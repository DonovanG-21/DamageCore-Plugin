// Fill out your copyright notice in the Description page of Project Settings.

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "Components/ActorComponent.h"
#include "Data/HealthSystemTypes.h"
#include "DamageableComponent.generated.h"


// ============================================================
// DELEGATES
// ============================================================

/**
 * Broadcast on all clients and the server (NetMulticast) every time Health changes,
 * whether from damage, raw damage, or restoration.
 *
 * @param NewHealth          The health value after the change, clamped to [0, MaxHealth].
 * @param bIncreased         True if Health increased (restoration), false if it decreased (damage).
 * @param LastReceivedDamage Metadata about the last damage event (instigator, tag, amount).
 * @param LastReceivedHealth Metadata about the last health restoration event.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FOnHealthChangedSignature, 
	float, NewHealth, 
	bool, bIncreased,
	FLastReceivedDamage, LastReceivedDamage,
	FLastReceivedHealth, LastReceivedHealth);

/**
 * Broadcast on the owning client only whenever this actor takes damage.
 * Not fired on the server or on other clients.
 *
 * @param DamageTaken  Actual health reduced after clamping.
 * @param HitResult    Hit context provided by the instigator (may be default if not supplied).
 * @param InstigatedBy The actor responsible for the damage.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnDamageTakenSignature, 
	float, DamageTaken, 
	const FHitResult&, HitResult,
	AActor*, InstigatedBy);

/**
 * Broadcast on the owning client only whenever this actor successfully deals damage.
 * Not fired on the server or on other clients.
 *
 * @param DamageDealt        Actual health reduced on the target after clamping.
 * @param HitResult          Hit context passed to TryToDealDamage (may be default if not supplied).
 * @param DamagedActor       The actor that received the damage.
 * @param DamagedActorNewLife The target's health value after the damage was applied.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FOnDamageDealtSignature, 
	float, DamageDealt, 
	const FHitResult&, HitResult,
	AActor*, DamagedActor,
	const float, DamagedActorNewLife);

/**
 * Broadcast on all clients and the server (NetMulticast) whenever this actor is hit.
 * Fired regardless of whether the instigator or the damaged actor is locally controlled.
 *
 * @param Damage       Actual health reduced on this actor after clamping.
 * @param HitResult    Hit context provided by the instigator (may be default if not supplied).
 * @param InstigatedBy The actor responsible for the hit.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnHitSignature, 
	float, Damage,
	const FHitResult&, HitResult,
	AActor*, InstigatedBy);

/**
 * Broadcast on all clients and the server (NetMulticast) when Health reaches zero.
 * LastReceivedDamage carries the bIsMortalDamage flag of the last damage source —
 * bind to this to decide whether to destroy, ragdoll, or enter a downed state.
 *
 * @param LastReceivedDamage Full metadata about the killing blow (instigator, tag, mortal flag).
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnHealthDepletedSignature, 
	FLastReceivedDamage, LastReceivedDamage);

// ============================================================

DECLARE_LOG_CATEGORY_EXTERN(LogDamageCore, Log, All);


/**
 * UDamageableComponent
 *
 * ActorComponent managing health, tag-based damage, and health restoration.
 *
 * ## Damage
 * - Damage values are resolved from a TMap<FGameplayTag, FDealDamageInfo> (DamageFactors),
 *   giving per-actor-class control over damage amount and mortality.
 * - If no entry is found in DamageFactors, a fallback lookup is performed in UniversalDamageTable.
 * - Incoming damage can be declaratively overridden per (tag + instigator class) via DamageReceptionOverrides,
 *   bypassing ModifyIncomingDamage entirely.
 *
 * ## Restoration
 * - Supports instant (fixed or percentage) and timer-driven auto-regeneration.
 * - Fixed tags (RESTORATION_FIXED_OLD and children) bypass the Restorations array.
 * - Auto-regen is triggered whenever Health decreases, if a RESTORATION_AUTO_OLD entry exists.
 *
 * ## Networking
 * - Health is replicated to all connections. All Health modifications are authority-only.
 * - Combat metadata (last instigator, last damage tag) is replicated to the owner only (kill-feed UI).
 * - TryToDealDamage and TryToHeal forward to the server via RPC when called on a client
 *   and bNotifyServerIfClient is true.
 *
 * ## Blueprint
 * - All public entry points are BlueprintCallable; delegates are BlueprintAssignable.
 * - ModifyIncomingDamage and ModifyOutgoingDamage are BlueprintNativeEvent and can be
 *   overridden in child Blueprints or C++ to customize damage without touching the actor.
 * - CanTakeDamage, CanDealDamage, CanDealDamageToActor, CanBeHealed, and CanDie
 *   are also BlueprintNativeEvent and provide extensible gate conditions.
 */
UCLASS(Blueprintable, ClassGroup=(Custom), meta=(BlueprintSpawnableComponent))
class DAMAGECORE_API UDamageableComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UDamageableComponent();

protected:
	virtual void BeginPlay() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;


	// ============================================================
	// PARAMETERS
	// ============================================================

public:

	/** Enables verbose logging for damage and restoration operations. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Parameters|Debug")
	bool bDebug = false;

	/** Maximum health value. Health is clamped to [0, MaxHealth] and initialized to this value on BeginPlay. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Parameters|Health")
	float MaxHealth = 100.f;
	
	/**
	 * Damage configuration keyed by Gameplay Tag.
	 * Each tag maps to a FDealDamageInfo containing per-class damage values and mortality flags.
	 * Used as the primary source in the damage pipeline. Falls back to UniversalDamageTable if no match is found.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Parameters|Factors|Damage")
	TMap<FGameplayTag, FDealDamageInfo> DamageFactors;
	
	/**
	 * Declarative reception overrides evaluated at the start of ModifyIncomingDamage.
	 * If the incoming DamageTag + instigator class matches an entry, the overridden value
	 * is returned immediately — child Blueprint/C++ logic in ModifyIncomingDamage is bypassed.
	 * Anti-cheat still holds: the instigator must have a valid entry for the tag
	 * before ModifyIncomingDamage is ever reached.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Parameters|Factors|Damage")
	TMap<FGameplayTag, FDealDamageInfo> DamageReceptionOverrides;

	/**
	 * Restoration configurations available to this actor.
	 * Each entry maps a Gameplay Tag to a heal amount and optional timer behavior.
	 * Entries whose RestorationTag matches RESTORATION_AUTO are triggered automatically
	 * whenever Health decreases (auto-regen).
	 * Fixed tags (RESTORATION_FIXED and children) bypass this array entirely.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Parameters|Factors|Restoration")
	TArray<FRestoreValue> Restorations;
	
	
	/**
	 * If true, OnDamageDealtDelegate is fired on the instigator's owning client
	 * whenever TryToDealDamage successfully reduces a target's health.
	 * Set to false to suppress the callback entirely (e.g. for AI-only attackers).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Parameters|Delegates")
	bool bNotifyInstigatorOnDamageDealt = true;


	// ============================================================
	// ATTRIBUTES
	// ============================================================

protected:

	/** Current health. Replicated to all connections. Authority-only writes. */
	UPROPERTY(Replicated, BlueprintReadWrite, Category="Health")
	float Health;

	/** Last actor that dealt damage. Replicated to owner only, used for kill-feed UI. */
	UPROPERTY(Replicated, BlueprintReadWrite, Category="Combat")
	AActor* LastAttackingInstigator = nullptr;

	/** Tag of the last damage type received. Replicated to owner only, used for kill-feed UI. */
	UPROPERTY(Replicated, BlueprintReadWrite, Category="Combat")
	FGameplayTag LastDamageTakenTag;
	
private:
	/**
	 * Global DataTable of universal damage types shared across all actors.
	 * Checked as a fallback when DamageFactors has no matching entry for the
	 * given tag and target class. Row struct: FDealDamageRow.
	 */
	UPROPERTY()
	TObjectPtr<UDataTable> UniversalDamageTable = nullptr;


	// ============================================================
	// GETTERS / STATES
	// ============================================================

public:

	/** Returns the current health value. */
	UFUNCTION(BlueprintCallable, Category="Health")
	FORCEINLINE float GetHealth() const { return Health; }

	/** Returns true if Health is above zero. */
	UFUNCTION(BlueprintCallable, Category="Health")
	FORCEINLINE bool HasHealth() const { return Health > 0.f; }

	/**
	 * Returns true if the owner can currently receive damage.
	 * Default: requires CanBeDamaged() on the owner and Health above zero.
	 * Override in Blueprint or C++ to add custom conditions (e.g. invincibility frames).
	 */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category="Health")
	bool CanTakeDamage() const;

	/**
	 * Returns true if this component's owner can currently initiate damage.
	 * Default: always true. Override to add conditions (e.g. stunned, disarmed).
	 */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category="Health")
	bool CanDealDamage() const;

	/**
	 * Returns true if this component's owner can deal damage to the given actor.
	 * Default: checks that ActorToDamage is valid and its UDamageableComponent
	 * reports CanTakeDamage(). Override to add team-check, range, or line-of-sight logic.
	 */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category="Health")
	bool CanDealDamageToActor(const AActor* ActorToDamage);

	/**
	 * Returns true if this component's owner can currently receive healing.
	 * Default: always true. Override to block healing under specific conditions.
	 * Note: not yet enforced inside RestoreHealth — enforcement is planned.
	 */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category="Health")
	bool CanBeHealed() const;

	/**
	 * Returns true if Health can reach zero (i.e. the owner can die).
	 * Default: always true. Override to implement immortal or near-death states.
	 * When false, Health is clamped to a minimum of 1 in SetHealth.
	 */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category="Health")
	bool CanDie() const;


	// ============================================================
	// DAMAGE
	// ============================================================

public:

	/**
	 * Attempts to deal damage to the target actor using the full server-authoritative pipeline:
	 *
	 * 1. Client guard: if called on a client —
	 *    - bNotifyServerIfClient == true  → forwards via ServerTryToDealDamage RPC, returns 0.
	 *    - bNotifyServerIfClient == false → no-op, returns 0.
	 * 2. CanDealDamage()           — instigator ability check.
	 * 3. CanDealDamageToActor()    — target validity + CanTakeDamage check.
	 * 4. FindDamageValue()         — anti-cheat: verifies the instigator has a registered
	 *                                damage entry for the tag + target class.
	 * 5. ModifyOutgoingDamage()    — instigator-side modifier (buffs, crits).
	 * 6. ModifyIncomingDamage()    — target-side modifier (armor, immunities).
	 * 7. ApplyDamage()             — commits the final value and dispatches delegates.
	 *
	 * @param ActorToDamage         The actor to damage.
	 * @param DamageTag             Tag identifying the damage type (must exist in DamageFactors or UniversalDamageTable).
	 * @param HitResult             Full hit context (optional). Forwarded to Modify hooks and delegates.
	 * @param bNotifyServerIfClient If true and called on a client, forwards the call to the server via RPC.
	 *                              Set to false when the server will already execute this logic
	 *                              to avoid duplicate damage events.
	 * @return Actual health reduced after clamping, or 0 if the pipeline was aborted.
	 */
	UFUNCTION(BlueprintCallable, Category="Health")
	float TryToDealDamage(AActor* ActorToDamage, const FGameplayTag& DamageTag, const FHitResult HitResult = FHitResult(), const bool bNotifyServerIfClient = false);

	/**
	 * Override this function to intercept and modify incoming damage before it is applied.
	 * Return a modified FDamageValue to reduce, amplify, or change the damage type.
	 * Return an invalid FDamageValue (DamageValue <= 0) to block all damage entirely.
	 *
	 * Declarative overrides in DamageReceptionOverrides are evaluated first —
	 * if a match is found, child logic is bypassed entirely.
	 *
	 * Default implementation: returns DamageValue unchanged.
	 */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category="Health")
	FDamageValue ModifyIncomingDamage(const FGameplayTag& DamageType, FDamageValue DamageValue, const FHitResult& HitResult, AActor* Instigator);

	/**
	 * Override this function to intercept and modify outgoing damage before it reaches the target.
	 * Return a modified FDamageValue to apply buffs, critical hit multipliers, or
	 * bonus damage against specific targets.
	 * Return an invalid FDamageValue (DamageValue <= 0) to cancel the damage entirely.
	 *
	 * Default implementation: returns DamageValue unchanged.
	 */
	UFUNCTION(BlueprintNativeEvent, BlueprintCallable, Category="Health")
	FDamageValue ModifyOutgoingDamage(const FGameplayTag& DamageType, FDamageValue DamageValue, const FHitResult& HitResult, AActor* ActorToDamage);

	/**
	 * Predicts the target's resulting health if this instigator deals damage now,
	 * without modifying any actual Health value.
	 * Mirrors the ModifyOutgoing → ModifyIncoming pipeline but skips the authority check
	 * and ApplyDamage call. Useful for client-side UI previews or ability targeting feedback.
	 *
	 * @param ActorToDamage  The actor to simulate damage against.
	 * @param DamageTag      Tag identifying the damage type.
	 * @param HitResult      Hit context forwarded to Modify hooks (optional).
	 * @param bClampedResult If true, the result is clamped to [0, MaxHealth].
	 * @param bReturnPercent If true, the result is expressed as a percentage of MaxHealth (0–100).
	 * @return Predicted health after damage. Returns the target's current Health if the pipeline
	 *         would abort (missing component, unregistered tag). Returns 0 if ActorToDamage is null.
	 */
	UFUNCTION(BlueprintCallable, Category="Health")
	float PredictHealthAfterDamage(AActor* ActorToDamage, const FGameplayTag& DamageTag, const FHitResult HitResult = FHitResult(), const bool bClampedResult = true, const bool bReturnPercent = false);
	
	/**
	 * Applies raw damage directly to this actor, bypassing tag lookup and anti-cheat.
	 * Intended for environment damage, fall damage, or sources without a DamageableComponent.
	 * Authority only.
	 *
	 * @param DamageValue     Amount and type of damage to apply.
	 *                        If bIsPercentDamage is true, the value is treated as a percentage of MaxHealth.
	 * @param Instigator      Optional actor responsible for the damage (stored for kill-feed).
	 * @return The raw damage computed before clamping. The actual Health reduction may be
	 *         smaller if Health was already near the minimum.
	 */
	UFUNCTION(BlueprintCallable, Category="Health")
	float ApplyRawDamage(const FDamageValue& DamageValue, AActor* Instigator = nullptr);


	// ============================================================
	// RESTORATION
	// ============================================================

public:

	/**
	 * Restores health on the target actor using a tag to identify the restoration type.
	 * Authority only. Client calls can be forwarded via RPC if bNotifyServerIfClient is true.
	 *
	 * - Tags matching RESTORATION_FIXED or its children apply a fixed amount directly,
	 *   bypassing the Restorations array.
	 * - All other tags are resolved from the Restorations array and may start a periodic regen timer.
	 *
	 * @param ActorToHeal           The actor to heal.
	 * @param RestorationTag        Tag identifying the restoration type.
	 * @param bNotifyServerIfClient If true and called on a client, forwards the call to the server via RPC.
	 * @return Amount of health effectively restored after clamping.
	 */
	UFUNCTION(BlueprintCallable, Category="Health")
	float TryToHeal(AActor* ActorToHeal, const FGameplayTag& RestorationTag, const bool bNotifyServerIfClient = false);


	// ============================================================
	// DELEGATES
	// ============================================================

public:

	/** Broadcast on every Health change, on both server and all clients (NetMulticast). */
	UPROPERTY(BlueprintAssignable)
	FOnHealthChangedSignature OnHealthChangedDelegate;

	/** Broadcast on the owning client whenever this actor takes damage. */
	UPROPERTY(BlueprintAssignable)
	FOnDamageTakenSignature OnDamageTakenDelegate;

	/** Broadcast on the owning client whenever this actor successfully deals damage. */
	UPROPERTY(BlueprintAssignable)
	FOnDamageDealtSignature OnDamageDealtDelegate;

	/** Broadcast on all clients whenever this actor is hit (NetMulticast). */
	UPROPERTY(BlueprintAssignable)
	FOnHitSignature OnHitDelegate;

	/** Broadcast on all clients when Health reaches zero (NetMulticast).
	 *  bShouldKillOwner reflects the bIsMortalDamage flag of the last damage source. */
	UPROPERTY(BlueprintAssignable)
	FOnHealthDepletedSignature OnHealthDepleted;


	// ============================================================
	// PRIVATE
	// ============================================================

private:

	/**
	 * Single authority-only entry point for all Health modifications.
	 * Clamps the new value, dispatches delegates via NetMulticast, and
	 * triggers auto-restoration if a RESTORATION_AUTO_OLD entry exists.
	 */
	void SetHealth(const float NewHealth);

	/** Records the instigator and damage tag of the most recent attack for kill-feed and mortality checks. */
	void UpdateLastAttackingData(AActor* ActorInstigator, const FGameplayTag& DamageTag);
	
	float ApplyDamage(AActor* InstigatedBy, const FGameplayTag& DamageTag, const FDamageValue& DamageToApply);
	
	float ApplyHealing(const FRestoreValue& HealToApply, AActor* Instigator = nullptr);

	/** Forwards TryToDealDamage to the server when bRequireAuthority is true on a client. */
	UFUNCTION(Server, Reliable)
	void ServerTryToDealDamage(AActor* ActorToDamage, const FGameplayTag& DamageTag, const FHitResult& HitResult);
	
	/** Forwards TryToHeal to the server when bRequireAuthority is true on a client. */
	UFUNCTION(Server, Reliable)
	void ServerTryToRestoreHealth(AActor* ActorToHeal, const FGameplayTag& RestorationTag);

	/**
	 * Searches DamageFactors for a value matching both the tag and the target actor class
	 * (including subclasses), then falls back to UniversalDamageTable.
	 * Returns a default-constructed (invalid) FDamageValue if no match is found.
	 */
	FDamageValue FindDamageValue(const FGameplayTag& DamageTag, TSubclassOf<AActor> ActorClassToDamage) const;

	/**
	 * Queries the last attacker's DamageFactors to determine whether this owner
	 * can be killed by the damage they dealt (bIsMortalDamage flag).
	 * Falls back to true (lethal) if no component or no matching entry is found.
	 */
	bool CanBeKilledByActor(const AActor* ActorToCheck);

	/**
	 * Authority-side dispatcher: calls DispatchHealthChangedDelegates on every connection
	 * via NetMulticast. Called by SetHealth after every authoritative Health modification.
	 */
	UFUNCTION(NetMulticast, Reliable)
	void NetMulticastDispatchHealthChangedDelegates(const float NewHealth, const float OldHealth, const FLastReceivedDamage& LastReceivedDamage, const FLastReceivedHealth& LastReceivedHealth);
	void DispatchHealthChangedDelegates(const float NewHealth, const float OldHealth, const FLastReceivedDamage& LastReceivedDamage, const FLastReceivedHealth& LastReceivedHealth);
	bool TryToDispatchHealthChangedDelegates(const float NewHealth, const float OldHealth, const FLastReceivedDamage& LastReceivedDamage, const FLastReceivedHealth& LastReceivedHealth);

	/** Notifies the owning client that this actor took damage. Fires OnDamageTakenDelegate. */
	UFUNCTION(Client, Reliable)
	void ClientNotifyDamageTaken(const float DamageTaken, const FHitResult& HitResult, AActor* InstigatedBy);

	/** Notifies the instigator's owning client that it successfully dealt damage. Fires OnDamageDealtDelegate. */
	UFUNCTION(Client, Reliable)
	void ClientNotifyDamageDealt(const float DamageDealt, const FHitResult& HitResult, AActor* DamagedActor, const float DamagedActorNewLife);

	/** Notifies all clients that a hit occurred on this actor. Fires OnHitDelegate. */
	UFUNCTION(NetMulticast, Reliable)
	void NetMulticastNotifyHit(const float Damage, const FHitResult& HitResult, AActor* InstigatedBy);


	// ---- Restoration helpers ----

	/** Returns a pointer to the FRestoreValue matching the given tag, or nullptr. */
	FRestoreValue GetRestoreValueFromTag(const FGameplayTag& RestoreTag, AActor* Instigator = nullptr);

	/** Converts a fixed restoration tag (QUARTER, HALF, FULL) to a concrete health amount. */
	float GetValueFromFixedTag(const FGameplayTag& FixedRestoreTag) const;


	// ---- Timer ----

	/** Handle for the active auto-restoration timer. Only one regen timer runs at a time. */
	UPROPERTY()
	FTimerHandle AutoRestorationTimerHandle;

	/** Starts a repeating restoration timer, cancelling any currently active one first. */
	void StartHealthRestorationTimer(const FRestoreValue& RestoreHealthInfo);

	/** Clears the active restoration timer if one is running. */
	void StopHealthRestorationTimer();

	/** Timer callback. Applies one regen tick and stops the timer when Health is zero or full. */
	void TimerHealthRestoration(FRestoreValue RestoreHealthInfo);
};