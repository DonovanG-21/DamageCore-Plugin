// Fill out your copyright notice in the Description page of Project Settings.

#include "Core/DamageableComponent.h"
#include "Net/UnrealNetwork.h"
#include "Engine/World.h"
#include "Engine/HitResult.h"
#include "TimerManager.h"


DEFINE_LOG_CATEGORY(LogDamageCore);

// Disable tick (unused) and enable replication by default.
// Attempts to load the shared UniversalDamageTable from the plugin content folder
// (/DamageCore/Data/DT_UniversalDamageFactors). Logs an error if the asset is missing.
UDamageableComponent::UDamageableComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
	
	static ConstructorHelpers::FObjectFinder<UDataTable> DamageTableFinder(
		TEXT("/DamageCore/Data/DT_UniversalDamageFactors.DT_UniversalDamageFactors"));

	if (DamageTableFinder.Succeeded())
	{
		UniversalDamageTable = DamageTableFinder.Object;
	}
	else
	{
		UE_LOG(LogDamageCore, Error,
			TEXT("UDamageableComponent - Failed to load UniversalDamageTable. "
				 "Ensure DT_UniversalDamageFactors exists in the plugin content folder."))
	}
}

// Authority only. Initializes Health to MaxHealth so the actor always starts at full health.
// If MaxHealth is misconfigured (<= 0), Health is clamped to 1 and a warning is logged.
void UDamageableComponent::BeginPlay()
{
	Super::BeginPlay();
	
	if (GetOwner()->HasAuthority())
	{
		Health = MaxHealth > 0.f ? MaxHealth : 1.f;
		
		if (MaxHealth <= 0.f)
		{
			UE_LOG(LogDamageCore, Warning, TEXT("UDamageableComponent::BeginPlay - Health has been set to 1 because MaxHealth <= 0. "))
		}
	}
}

// Health is replicated to all connections (DOREPLIFETIME).
// LastAttackingInstigator and LastDamageTakenTag are replicated to the owner only
// and only when their value changes (COND_OwnerOnly | REPNOTIFY_OnChanged).
void UDamageableComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(ThisClass, Health);

	DOREPLIFETIME_CONDITION_NOTIFY(ThisClass, LastAttackingInstigator, COND_OwnerOnly, REPNOTIFY_OnChanged);
	DOREPLIFETIME_CONDITION_NOTIFY(ThisClass, LastDamageTakenTag, COND_OwnerOnly, REPNOTIFY_OnChanged);
}


#pragma region SETTERS

// Authority-only entry point for all Health modifications.
// Clamps the incoming value to [0, MaxHealth] (or [1, MaxHealth] if CanDie() is false).
// Note: delegates are NOT dispatched here — callers (ApplyDamage, ApplyHealing) are
// responsible for calling TryToDispatchHealthChangedDelegates after SetHealth.
void UDamageableComponent::SetHealth(const float NewHealth)
{
	if (!GetOwner()->HasAuthority())
	{
		UE_LOG(LogDamageCore, Warning, TEXT("UDamageableComponent::SetHealth - To call this function you must have Authority. "))
		return;
	}
	
	const float OldHealth = Health;
	const float MinHealth = CanDie() ? 0.f : 1.f;
	Health = FMath::Clamp(NewHealth, MinHealth, MaxHealth);
}

// Stores the instigator and damage tag of the most recent attack.
// Used by kill-feed widgets and CanBeKilledByActor for mortality resolution.
void UDamageableComponent::UpdateLastAttackingData(AActor* ActorInstigator, const FGameplayTag& DamageTag)
{
	LastAttackingInstigator = ActorInstigator;
	LastDamageTakenTag = DamageTag;
}

#pragma endregion

#pragma region GETTERS

// Searches the Restorations array for an entry matching RestoreTag (exact match).
// If Instigator is valid and has a DamageableComponent, the search is performed
// on the instigator's Restorations array instead of this component's.
// Returns a default-constructed (invalid) FRestoreValue and logs a warning if no match is found.
FRestoreValue UDamageableComponent::GetRestoreValueFromTag(const FGameplayTag& RestoreTag, AActor* Instigator)
{
	if (IsValid(Instigator))
	{
		const UDamageableComponent* InstigatorDamageableComp = Instigator->FindComponentByClass<UDamageableComponent>();
		if (IsValid(InstigatorDamageableComp))
		{
			for (auto& Entry : InstigatorDamageableComp->Restorations)
			{
				if (RestoreTag.MatchesTagExact(Entry.RestorationTag))
				{
					return Entry;
				}
			}
			
			UE_LOG(LogDamageCore, Warning,
				TEXT("UDamageableComponent::GetRestoreValueFromTag - No entry found for tag '%s' in '%s'."),
				*RestoreTag.ToString(),
				*Instigator->GetName())
			
			return FRestoreValue();
		}
	}
	
	for (auto& Entry : Restorations)
	{
		if (RestoreTag.MatchesTagExact(Entry.RestorationTag))
		{
			return Entry;
		}
	}

	UE_LOG(LogDamageCore, Warning,
		TEXT("UDamageableComponent::GetRestoreValueFromTag - No entry found for tag '%s' in '%s'."),
		*RestoreTag.ToString(),
		*GetOwner()->GetName())

	return FRestoreValue();
}

// Maps the three predefined fixed restoration tags to concrete health amounts:
//   RESTORATION_FIXED_QUARTER → MaxHealth / 4
//   RESTORATION_FIXED_HALF    → MaxHealth / 2
//   RESTORATION_FIXED_FULL    → MaxHealth
// Returns 0 and logs a warning for any unrecognized tag.
float UDamageableComponent::GetValueFromFixedTag(const FGameplayTag& FixedRestoreTag) const
{
	if (FixedRestoreTag.MatchesTagExact(RESTORATION_FIXED_QUARTER)) { return MaxHealth / 4.f; }
	if (FixedRestoreTag.MatchesTagExact(RESTORATION_FIXED_HALF))    { return MaxHealth / 2.f; }
	if (FixedRestoreTag.MatchesTagExact(RESTORATION_FIXED_FULL))    { return MaxHealth; }

	UE_LOG(LogDamageCore, Warning,
		TEXT("UDamageableComponent::GetValueFromFixedTag - No matching fixed tag for '%s'. Returning 0."),
		*FixedRestoreTag.ToString())

	return 0.f;
}

// Queries the last attacker's DamageFactors to check the bIsMortalDamage flag
// for the damage type stored in LastDamageTakenTag.
// Returns false if ActorToCheck is invalid.
// Falls back to true (lethal) if the attacker has no DamageableComponent or no matching entry.
bool UDamageableComponent::CanBeKilledByActor(const AActor* ActorToCheck)
{
	if (!IsValid(ActorToCheck)) { return false; }

	if (UDamageableComponent* AttackerComponent = ActorToCheck->FindComponentByClass<UDamageableComponent>())
	{
		const FDamageValue FoundDamageValue = AttackerComponent->FindDamageValue(LastDamageTakenTag, GetOwner()->GetClass());
		if (FoundDamageValue.IsValid())
		{
			return FoundDamageValue.bIsMortalDamage;
		}
	}

	return true;
}

#pragma endregion

#pragma region STATES

// ===================== OVERRIDABLE ==============================

// Requires both GetOwner()->CanBeDamaged() == true AND Health > 0.
bool UDamageableComponent::CanTakeDamage_Implementation() const
{
	return GetOwner()->CanBeDamaged() && HasHealth();
}

// Default: always returns true. Override to add instigator-side conditions.
bool UDamageableComponent::CanDealDamage_Implementation() const
{
	return true;
}

// Returns true only if ActorToDamage is valid and its DamageableComponent reports CanTakeDamage().
bool UDamageableComponent::CanDealDamageToActor_Implementation(const AActor* ActorToDamage)
{
	if (!IsValid(ActorToDamage))
	{
		return false;
	}
	
	if (const UDamageableComponent* ActorToDamageComp = ActorToDamage->FindComponentByClass<UDamageableComponent>())
	{
		if (ActorToDamageComp->CanTakeDamage())
		{
			return true;
		}
	}
	
	return false;
}

// Default: always returns true. Override to block healing (currently unenforced in TryToHeal).
bool UDamageableComponent::CanBeHealed_Implementation() const
{
	return true;
}

// Default: always returns true. When false, SetHealth clamps to a minimum of 1.
bool UDamageableComponent::CanDie_Implementation() const
{
	return true;
}

#pragma endregion

#pragma region METHODS

// ===================== DAMAGE ==============================

/**
 * Full server-authoritative damage pipeline. See header for step-by-step description.
 *
 * Implementation notes:
 * - CanTakeDamage() is checked twice: once in CanDealDamageToActor() (step 3) and
 *   once again after FindDamageValue() (step 4). The second check guards against a
 *   race condition where the target becomes immune between the two calls.
 * - ClientNotifyDamageDealt is currently sent unconditionally due to `|| true`
 *   in the net-connection guard (see ⚠️ Incohérences).
 * - HitResult actor mismatch is non-fatal: a warning is logged but damage proceeds.
 */
float UDamageableComponent::TryToDealDamage(AActor* ActorToDamage, const FGameplayTag& DamageTag, const FHitResult HitResult, const bool bNotifyServerIfClient)
{	
	// Forward to the server if authority is required and this is a client.
	if (!GetOwner()->HasAuthority())
	{
		if (bNotifyServerIfClient)
		{
			ServerTryToDealDamage(ActorToDamage, DamageTag, HitResult);
		}
		
		return 0.f;
	}
	
	if (!CanDealDamage())
    {
        if (bDebug)
        {
            UE_LOG(LogDamageCore, Warning,
                TEXT("UDamageableComponent::TryToDealDamage - '%s' cannot deal damage."),
                *GetOwner()->GetName())
        }
        return 0.f;
    }
	
	if (!CanDealDamageToActor(ActorToDamage))
	{
		if (bDebug)
		{
			UE_LOG(LogDamageCore, Warning,
				TEXT("UDamageableComponent::TryToDealDamage - Can't deal damage to targeted actor."))
		}
			
		return 0.f;
	}
	
	UDamageableComponent* TargetComponent = ActorToDamage->FindComponentByClass<UDamageableComponent>();
	if (!TargetComponent)
	{
		if (bDebug)
		{
			UE_LOG(LogDamageCore, Warning,
				TEXT("UDamageableComponent::TryToDealDamage - '%s' does not have a UDamageableComponent."),
				*ActorToDamage->GetName())
		}
		return 0.f;
	}
	
	if (HitResult.IsValidBlockingHit() && HitResult.GetActor() != ActorToDamage)
	{
		UE_LOG(LogDamageCore, Warning,
			TEXT("TryToDealDamage - HitResult actor mismatch with ActorToDamage. "
				 "HitResult data may be inaccurate."))
	}

    // Anti-cheat: verify this instigator is registered to deal this damage type to the target class.
    const FDamageValue DamageValue = FindDamageValue(DamageTag, ActorToDamage->GetClass());
    if (!DamageValue.IsValid())
    {
        if (bDebug)
        {
            UE_LOG(LogDamageCore, Warning,
                TEXT("UDamageableComponent::TryToDealDamage - No damage entry for tag '%s' targeting '%s'."),
                *DamageTag.ToString(), *ActorToDamage->GetName())
        }
        return 0.f;
    }

    if (!TargetComponent->CanTakeDamage())
    {
        if (bDebug)
        {
            UE_LOG(LogDamageCore, Warning,
                TEXT("UDamageableComponent::TryToDealDamage - '%s' cannot take damage."),
                *ActorToDamage->GetName())
        }
        return 0.f;
    }

    // Give the instigator a chance to modify the damage before it is sent (buffs, critical hits...).
    const FDamageValue ModifiedDamageValue = ModifyOutgoingDamage(DamageTag, DamageValue, HitResult, ActorToDamage);
	if (!ModifiedDamageValue.IsValid())
	{
		if (bDebug)
		{
			UE_LOG(LogDamageCore, Warning,
				TEXT("UDamageableComponent::TryToDealDamage - Damage fully blocked by ModifyOutgoingDamage on '%s'."),
				*GetOwner()->GetName())
		}
		return 0.f;
	}

    // Give the target a chance to intercept and modify the incoming damage (armor, immunities...).
    // An invalid return value means the damage was fully blocked.
    const FDamageValue FinalDamage = TargetComponent->ModifyIncomingDamage(DamageTag, ModifiedDamageValue, HitResult, GetOwner());
    if (!FinalDamage.IsValid())
    {
        if (bDebug)
        {
            UE_LOG(LogDamageCore, Warning,
                TEXT("UDamageableComponent::TryToDealDamage - Damage fully blocked by ModifyIncomingDamage on '%s'."),
                *ActorToDamage->GetName())
        }
        return 0.f;
    }
	
	const float AppliedDamage = TargetComponent->ApplyDamage(GetOwner(), DamageTag, FinalDamage);
	
	if (AppliedDamage > 0.f)
	{
		// GetNetConnection() != nullptr means the actor is owned by a remote client
		// — safe to send a Client RPC. Local/server-owned actors have no connection.
		if (GetOwner()->GetNetConnection())
		{
			ClientNotifyDamageDealt(AppliedDamage, HitResult, ActorToDamage, TargetComponent->GetHealth());
		}
		
		if (TargetComponent->GetOwner()->GetNetConnection())
		{
			TargetComponent->ClientNotifyDamageTaken(AppliedDamage, HitResult, GetOwner());
		}
		
		TargetComponent->NetMulticastNotifyHit(AppliedDamage, HitResult, GetOwner());
	}

    return AppliedDamage;
}

// Server RPC relay. Calls TryToDealDamage with default bNotifyServerIfClient (false)
// to prevent any risk of infinite RPC recursion.
void UDamageableComponent::ServerTryToDealDamage_Implementation(AActor* ActorToDamage, const FGameplayTag& DamageTag, const FHitResult& HitResult)
{
	TryToDealDamage(ActorToDamage, DamageTag, HitResult);
}

/**
 * Commits a validated FDamageValue to this actor's Health.
 * Called exclusively at the end of TryToDealDamage — do not call directly.
 *
 * Steps:
 * 1. Authority guard.
 * 2. Converts percent-based damage to an absolute HP amount (DamageValue * MaxHealth / 100).
 * 3. Calls UpdateLastAttackingData then SetHealth.
 * 4. If health effectively decreased: dispatches OnHealthChanged delegates via
 *    TryToDispatchHealthChangedDelegates, then triggers RESTORATION_AUTO if applicable.
 *
 * @param InstigatedBy  Actor responsible for the damage. Must be valid.
 * @param DamageTag     Tag stored for kill-feed and mortality checks.
 * @param DamageToApply Validated damage value (must pass IsValid()).
 * @return Effective health reduction (OldHealth - Health after clamping). 0 on failure.
 */
float UDamageableComponent::ApplyDamage(AActor* InstigatedBy, const FGameplayTag& DamageTag, const FDamageValue& DamageToApply)
{
	if (!GetOwner()->HasAuthority())
	{
		UE_LOG(LogDamageCore, Warning, TEXT("UDamageableComponent::ApplyDamage - Owner does not have authority."))
		return 0.f;
	}
	
	if (!DamageToApply.IsValid() || !IsValid(InstigatedBy))
	{
		return 0.f;
	}
	
	const float OldHealth = Health;

	// Convert percent-based damage to a concrete health amount before applying.
	const float DamageToDeal = DamageToApply.bIsPercentDamage
		? DamageToApply.DamageValue * MaxHealth / 100.f
		: DamageToApply.DamageValue;

	// Record combat context before modifying Health so delegates have accurate metadata.
	UpdateLastAttackingData(InstigatedBy, DamageTag);
	SetHealth(Health - DamageToDeal);
	
	const float DamageDealt = OldHealth - Health;
	
	if (DamageDealt > 0.f)
	{
		// Dispatch damage relative delegates
		const FLastReceivedDamage LastReceivedDamage = FLastReceivedDamage(DamageToApply , InstigatedBy);
		TryToDispatchHealthChangedDelegates(Health, OldHealth, LastReceivedDamage, FLastReceivedHealth());
		
		if (HasHealth())
		{
			// Find if damaged actor have auto restore health entry
			for (const auto& Entry : Restorations)
			{
				if (Entry.RestorationTag.MatchesTagExact(RESTORATION_AUTO))
				{
					ApplyHealing(Entry);
				}
			}
		}
	}

	return DamageDealt;
}

/**
 * Applies raw damage directly, bypassing tag lookup, anti-cheat, and Modify hooks.
 * Intended for environment/fall damage or damage sources without a DamageableComponent.
 * Uses DAMAGE_RAW as the stored tag for kill-feed metadata.
 *
 * Checks in order: authority, CanBeDamaged(), DamageValue.IsValid().
 * Converts percent-based damage identically to ApplyDamage.
 * Triggers RESTORATION_AUTO on the same condition as ApplyDamage.
 *
 * @return Effective health reduction after clamping.
 */
float UDamageableComponent::ApplyRawDamage(const FDamageValue& DamageValue, AActor* Instigator)
{
	if (!GetOwner()->HasAuthority())
	{
		UE_LOG(LogDamageCore, Warning, TEXT("UDamageableComponent::ApplyRawDamage - Owner does not have authority."))
		return 0.f;
	}
	if (!GetOwner()->CanBeDamaged())
	{
		UE_LOG(LogDamageCore, Warning, TEXT("UDamageableComponent::ApplyRawDamage - '%s' cannot be damaged."),
			*GetOwner()->GetName())
		return 0.f;
	}
	if (!DamageValue.IsValid())
	{
		UE_LOG(LogDamageCore, Warning, TEXT("UDamageableComponent::ApplyRawDamage - DamageValue is not valid."))
		return 0.f;
	}

	UpdateLastAttackingData(Instigator, DAMAGE_RAW);

	const float OldHealth = Health;
	const float FinalDamage = 
		DamageValue.bIsPercentDamage ? 
		DamageValue.DamageValue * MaxHealth / 100.f : 
		DamageValue.DamageValue;
	SetHealth(Health - FinalDamage);
	
	const float DamageDealt = OldHealth - Health;
	
	if (DamageDealt > 0.f)
	{
		// Dispatch damage relative delegates
		const FLastReceivedDamage LastReceivedDamage = FLastReceivedDamage(DamageValue , Instigator);
		TryToDispatchHealthChangedDelegates(Health, OldHealth, LastReceivedDamage, FLastReceivedHealth());
		
		// Find if damaged actor have auto restore health entry
		if (HasHealth())
		{
			for (const auto& Entry : Restorations)
			{
				if (Entry.RestorationTag.MatchesTagExact(RESTORATION_AUTO))
				{
					ApplyHealing(Entry);
				}
			}
		}
	}
	
	return DamageDealt;
}



// ===================== RESTORATION ============================

/**
 * Entry point for health restoration. See header for tag routing description.
 *
 * Implementation notes:
 * - Client guard mirrors TryToDealDamage: forwards via RPC if bNotifyServerIfClient,
 *   otherwise returns 0.
 * - Fixed tags are matched with MatchesTag (not exact), covering all RESTORATION_FIXED children.
 * - Non-fixed tags are resolved from the *instigator's* Restorations array via
 *   GetRestoreValueFromTag (Instigator = GetOwner()).
 *
 * @return Amount of health effectively added after clamping (0 if aborted,
 *         -1 if a timer was started instead of an instant heal).
 */
float UDamageableComponent::TryToHeal(AActor* ActorToHeal, const FGameplayTag& RestorationTag, const bool bNotifyServerIfClient)
{
	if (!IsValid(ActorToHeal))
	{
		UE_LOG(LogDamageCore, Warning, TEXT("UDamageableComponent::TryToHeal - Failed to restore heal because ActorToHeal is not valid"))
		return 0.f;
	}
	if (!GetOwner()->HasAuthority())
	{
		if (bNotifyServerIfClient)
		{
			ServerTryToRestoreHealth(ActorToHeal, RestorationTag);
		}
		
		return 0.f;
	}
	
	UDamageableComponent* ActorToHealDamageableComp =  ActorToHeal->FindComponentByClass<UDamageableComponent>();
	if (IsValid(ActorToHealDamageableComp) && ActorToHealDamageableComp->CanBeHealed())
	{
		const FRestoreValue RestoreValue = RestorationTag.MatchesTag(RESTORATION_FIXED) ?
		FRestoreValue(ActorToHealDamageableComp->GetValueFromFixedTag(RestorationTag), false) : 
		GetRestoreValueFromTag(RestorationTag);
	
		return ActorToHealDamageableComp->ApplyHealing(RestoreValue, GetOwner());
	}
	
	return 0.f;	
}

void UDamageableComponent::ServerTryToRestoreHealth_Implementation(AActor* ActorToHeal, const FGameplayTag& RestorationTag)
{
	TryToHeal(ActorToHeal, RestorationTag);
}

/**
 * Applies an instant heal or starts a regen timer.
 *
 * - If HealToApply.bIsTimer is true: delegates to StartHealthRestorationTimer and returns -1.
 * - Otherwise: converts percent values (HealthToAdd / 100 * MaxHealth), calls SetHealth,
 *   and dispatches OnHealthChanged delegates via TryToDispatchHealthChangedDelegates.
 *
 * Authority only.
 *
 * @return Effective health gained (Health - OldHealth after clamping).
 *         Returns -1 if a timer was started. Returns 0 if no health changed.
 */
float UDamageableComponent::ApplyHealing(const FRestoreValue& HealToApply, AActor* Instigator)
{
	if (!GetOwner()->HasAuthority())
	{
		UE_LOG(LogDamageCore, Warning, TEXT("UDamageableComponent::ApplyHealing - Owner does not have authority."))
		return 0.f;
	}
	if (!HealToApply.IsValid())
	{
		UE_LOG(LogDamageCore, Warning, TEXT("UDamageableComponent::ApplyHealing - HealToApply is not valid."))
		return 0.f;
	}
	if (HealToApply.bIsTimer)
	{
		// [RETURN] If this is a timer
		StartHealthRestorationTimer(HealToApply);
		return -1.f;
	}
	
	const float OldHealth = Health;
	
	const float HealthToAdd = HealToApply.bIsPercentValue ?
				HealToApply.HealthToAdd / 100.f * MaxHealth :
				HealToApply.HealthToAdd;
	
	if (HealthToAdd > 0.f)
	{
		SetHealth(Health + HealthToAdd);
		
		if (Health != OldHealth)
		{
			const FLastReceivedHealth LastReceivedHealth = FLastReceivedHealth(HealToApply , Instigator);
			TryToDispatchHealthChangedDelegates(Health, OldHealth, FLastReceivedDamage(), LastReceivedHealth);
			
			return Health - OldHealth;
		}
	}
	
	return 0.f;
}



// ===================== PREDICTION ============================

// Mirrors the ModifyOutgoing → ModifyIncoming pipeline without applying damage.
// Returns the target's current Health unchanged on any failure so the caller
// can detect that no damage would have been dealt. Returns 0 if the target actor is invalid.
float UDamageableComponent::PredictHealthAfterDamage(AActor* ActorToDamage, const FGameplayTag& DamageTag, const FHitResult HitResult, const bool bClampedResult, const bool bReturnPercent)
{
	if (!IsValid(ActorToDamage))
	{
		UE_LOG(LogDamageCore, Warning, TEXT("UDamageableComponent::PredictHealthAfterDamage - ActorToDamage is not valid."))
		return 0.f;
	}
	
	if (!CanDealDamageToActor(ActorToDamage))
	{
		UE_LOG(LogDamageCore, Warning,
			TEXT("UDamageableComponent::PredictHealthAfterDamage - CanDealDamageToActor return false to '%s'."),
			*ActorToDamage->GetName())
		return 0.f;
	}
	
	UDamageableComponent* ActorToDamageableComp = ActorToDamage->FindComponentByClass<UDamageableComponent>();

	const FDamageValue FoundDamageValue = FindDamageValue(DamageTag, ActorToDamage->GetClass());
	if (!FoundDamageValue.IsValid())
	{
		UE_LOG(LogDamageCore, Warning,
			TEXT("UDamageableComponent::PredictHealthAfterDamage - Tag '%s' not found in '%s' to damage '%s'."),
			*DamageTag.ToString(), *GetOwner()->GetName() ,*ActorToDamage->GetName())
		return ActorToDamageableComp->GetHealth();
	}
	
	const FDamageValue ModifiedDamageValue = ModifyOutgoingDamage(DamageTag, FoundDamageValue, HitResult, ActorToDamage);
	if (!ModifiedDamageValue.IsValid())
	{
		if (bDebug)
		{
			UE_LOG(LogDamageCore, Warning,
				TEXT("UDamageableComponent::PredictHealthAfterDamage - Damage fully blocked by ModifyOutgoingDamage on '%s'."),
				*GetOwner()->GetName())
		}
		return ActorToDamageableComp->GetHealth();
	}
	
	const FDamageValue FinalDamage = ActorToDamageableComp->ModifyIncomingDamage(DamageTag, ModifiedDamageValue, HitResult, GetOwner());
	if (!FinalDamage.IsValid())
	{
		if (bDebug)
		{
			UE_LOG(LogDamageCore, Warning,
				TEXT("UDamageableComponent::PredictHealthAfterDamage - Damage fully blocked by ModifyIncomingDamage on '%s'."),
				*ActorToDamage->GetName())
		}
		return ActorToDamageableComp->GetHealth();
	}
	
	const float DamageToDeal = FinalDamage.bIsPercentDamage
		? FinalDamage.DamageValue * ActorToDamageableComp->MaxHealth / 100.f
		: FinalDamage.DamageValue;

	const float NewHealth = bClampedResult
		? FMath::Clamp(ActorToDamageableComp->GetHealth() - DamageToDeal, 0.f, ActorToDamageableComp->MaxHealth)
		: ActorToDamageableComp->GetHealth() - DamageToDeal;

	return bReturnPercent ? NewHealth / ActorToDamageableComp->MaxHealth * 100.f : NewHealth;
}

// Looks up DamageFactors by tag, then iterates DamageValues to find an entry
// whose ActorClass is a parent of (or equal to) the target class.
// Falls back to UniversalDamageTable if DamageFactors has no match.
// In UniversalDamageTable, bIsClassAgnostic rows match any target class (first value returned).
// Returns a default-constructed (invalid) FDamageValue if neither source has a match.
FDamageValue UDamageableComponent::FindDamageValue(const FGameplayTag& DamageTag, const TSubclassOf<AActor> ActorClassToDamage) const
{
	if (!DamageFactors.IsEmpty())
	{
		if (const FDealDamageInfo* DealDamageInfo = DamageFactors.Find(DamageTag))
		{
			for (const auto& DamageValue : DealDamageInfo->DamageValues)
			{
				if (ActorClassToDamage->IsChildOf(DamageValue.ActorClass))
				{
					return DamageValue;
				}
			}
		}
	}
	
	if (IsValid(UniversalDamageTable))
	{
		TArray<FDealDamageRow*> Rows;
		UniversalDamageTable->GetAllRows(TEXT("FindDamageValue"), Rows);
		for (const FDealDamageRow* Row : Rows)
		{
			if (Row && Row->DamageTag.MatchesTagExact(DamageTag) && !Row->DamageValues.IsEmpty())
			{
				if (Row->bIsClassAgnostic)
				{
					return Row->DamageValues[0];
				}
				
				for (const auto& DamageValue : Row->DamageValues)
				{
					if (ActorClassToDamage->IsChildOf(DamageValue.ActorClass))
					{
						return DamageValue;
					}
				}
			}
			
		}
	}

	return FDamageValue();
}



// ===================== OVERRIDABLE ==============================

// Default pass-through. Checks DamageReceptionOverrides first (tag + instigator class exact match).
// If a declarative override is found, returns it immediately, bypassing all child logic.
// Otherwise returns DamageValue unchanged.
FDamageValue UDamageableComponent::ModifyIncomingDamage_Implementation(const FGameplayTag& DamageType, FDamageValue DamageValue, const FHitResult& HitResult, AActor* Instigator)
{
	// Declarative override check — runs before any child logic.
	// If the DamageTag + instigator class combination is registered,
	// return the override immediately without calling child implementations.
	if (const FDealDamageInfo* Override = DamageReceptionOverrides.Find(DamageType))
	{
		for (const auto& Element : Override->DamageValues)
		{
			if (IsValid(Instigator) && Instigator->IsA(Element.ActorClass))
			{
				return Element;
			}
		}
	}

	// Child Blueprint or C++ logic runs only if no declarative override matched.
	return DamageValue;
}


// Default pass-through. Returns DamageValue unchanged.
// Override to apply buffs, crits, or cancel damage (return invalid FDamageValue).
FDamageValue UDamageableComponent::ModifyOutgoingDamage_Implementation(const FGameplayTag& DamageType, FDamageValue DamageValue, const FHitResult& HitResult, AActor* ActorToDamage)
{
	return DamageValue;
}



// ===================== DELEGATES ==============================

// Validates that the appropriate metadata struct is populated before dispatching.
// OnHealthChanged fires when NewHealth != OldHealth with a valid LastReceivedDamage (decrease)
// or LastReceivedHealth (increase).
// Logs a warning and returns false if neither condition is met (e.g. both structs are default).
bool UDamageableComponent::TryToDispatchHealthChangedDelegates(const float NewHealth, const float OldHealth, const FLastReceivedDamage& LastReceivedDamage, const FLastReceivedHealth& LastReceivedHealth)
{
	if (NewHealth > OldHealth && LastReceivedHealth.IsValid())
	{
		NetMulticastDispatchHealthChangedDelegates(NewHealth, OldHealth, LastReceivedDamage, LastReceivedHealth);
		return true;
	}
	
	if (NewHealth < OldHealth && LastReceivedDamage.IsValid())
	{
		NetMulticastDispatchHealthChangedDelegates(NewHealth, OldHealth, LastReceivedDamage, LastReceivedHealth);
		return true;
	}
	
	UE_LOG(LogDamageCore, Warning, TEXT("UDamageableComponent::TryToDispatchHealthChangedDelegates - Failed to call health delegates because %hs is not valid"),
		NewHealth > OldHealth ? "LastReceivedHealth" : "LastReceivedDamage")
	
	return false;
}

// NetMulticast relay — runs DispatchHealthChangedDelegates on every connection.
void UDamageableComponent::NetMulticastDispatchHealthChangedDelegates_Implementation(const float NewHealth, const float OldHealth, const FLastReceivedDamage& LastReceivedDamage, const FLastReceivedHealth& LastReceivedHealth)
{
	DispatchHealthChangedDelegates(NewHealth, OldHealth, LastReceivedDamage, LastReceivedHealth);
}

// Broadcasts OnHealthChangedDelegate on every call.
// Broadcasts OnHealthDepleted when NewHealth <= 0.
// bIncreased is true when NewHealth >= OldHealth (restoration), false otherwise (damage).
void UDamageableComponent::DispatchHealthChangedDelegates(const float NewHealth, const float OldHealth, const FLastReceivedDamage& LastReceivedDamage, const FLastReceivedHealth& LastReceivedHealth)
{
	if (OnHealthChangedDelegate.IsBound())
	{
		OnHealthChangedDelegate.Broadcast(NewHealth, NewHealth >= OldHealth, LastReceivedDamage, LastReceivedHealth);
	}

	if (NewHealth <= 0.f && OnHealthDepleted.IsBound())
	{
		OnHealthDepleted.Broadcast(LastReceivedDamage);
	}
}

// Client RPC. Fires OnDamageTakenDelegate on the damaged actor's owning client.
void UDamageableComponent::ClientNotifyDamageTaken_Implementation(const float DamageTaken, const FHitResult& HitResult, AActor* InstigatedBy)
{
	if (OnDamageTakenDelegate.IsBound())
	{
		OnDamageTakenDelegate.Broadcast(DamageTaken, HitResult, InstigatedBy);
	}
}

// Client RPC. Fires OnDamageDealtDelegate on the instigator's owning client.
// Also relays the broadcast to the instigator's own DamageableComponent if
// bNotifyInstigatorOnDamageDealt is true and the owner has a different Instigator set.
void UDamageableComponent::ClientNotifyDamageDealt_Implementation(const float DamageDealt, const FHitResult& HitResult, AActor* DamagedActor , const float DamagedActorNewLife)
{
	if (OnDamageDealtDelegate.IsBound())
	{
		OnDamageDealtDelegate.Broadcast(DamageDealt, HitResult, DamagedActor, DamagedActorNewLife);
	}
	
	if (bNotifyInstigatorOnDamageDealt && IsValid(GetOwner()->GetInstigator()) && GetOwner()->GetInstigator() != GetOwner())
	{
		const UDamageableComponent* InstigatorDamageableComp = GetOwner()->GetInstigator()->FindComponentByClass<UDamageableComponent>();
		if (IsValid(InstigatorDamageableComp) && InstigatorDamageableComp->OnDamageDealtDelegate.IsBound())
		{
			InstigatorDamageableComp->OnDamageDealtDelegate.Broadcast(DamageDealt, HitResult, DamagedActor, DamagedActorNewLife);
		}
	}
}

// NetMulticast. Fires OnHitDelegate on all connections.
void UDamageableComponent::NetMulticastNotifyHit_Implementation(const float Damage, const FHitResult& HitResult, AActor* InstigatedBy)
{
	if (OnHitDelegate.IsBound())
	{
		OnHitDelegate.Broadcast(Damage, HitResult, InstigatedBy);
	}
}


// ===================== TIMERS ==============================

// Cancels any active regen timer before starting a new one (prevents overlapping ticks).
// The timer is non-repeating (bLoop = false) with an initial delay of RestoreTimerDelay.
// Each tick re-triggers itself via TimerHealthRestoration if conditions are still valid.
void UDamageableComponent::StartHealthRestorationTimer(const FRestoreValue& RestoreHealthInfo)
{
	if (GetWorld()->GetTimerManager().IsTimerActive(AutoRestorationTimerHandle))
	{
		StopHealthRestorationTimer();
	}

	const FTimerDelegate Delegate = FTimerDelegate::CreateUObject(this, &UDamageableComponent::TimerHealthRestoration, RestoreHealthInfo);
	GetWorld()->GetTimerManager().SetTimer(
		AutoRestorationTimerHandle,
		Delegate,
		RestoreHealthInfo.RestoreTimerDelay,
		false,
		-1);
}

// Clears AutoRestorationTimerHandle if the timer is currently active. No-op otherwise.
void UDamageableComponent::StopHealthRestorationTimer()
{
	if (GetWorld()->GetTimerManager().IsTimerActive(AutoRestorationTimerHandle))
	{
		GetWorld()->GetTimerManager().ClearTimer(AutoRestorationTimerHandle);
	}
}

// Timer callback for regen ticks. Exits early if Health is 0 or already at MaxHealth.
// Clears bIsTimer on the local copy before calling ApplyHealing to prevent re-entering
// the timer branch.
void UDamageableComponent::TimerHealthRestoration(FRestoreValue RestoreHealthInfo)
{
	if (!HasHealth() || Health >= MaxHealth)
	{
		return;
	}
	
	RestoreHealthInfo.bIsTimer = false;
	ApplyHealing(RestoreHealthInfo);
}

#pragma endregion