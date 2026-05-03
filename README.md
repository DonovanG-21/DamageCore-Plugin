# DamageableComponent — README

***

## Overview

`UDamageableComponent` is a plug-and-play Unreal Engine **ActorComponent** that handles all health logic for any `AActor`: tag-based damage, raw damage, health restoration, auto-regeneration, and death. It is fully **multiplayer-ready**, **Blueprint-extensible**, and designed to keep damage logic out of your actors entirely.

**Engine version:** Unreal Engine 5.x
**Plugin module:** `DamageCore`
**Replication:** Full (authority-only writes, NetMulticast delegates, owner-only combat metadata)

***

## Table of Contents

1. [Installation](#installation)
2. [Quick Start](#quick-start)
3. [Configuration](#configuration)
4. [Damage System](#damage-system)
5. [Restoration System](#restoration-system)
6. [Prediction](#prediction)
7. [Delegates](#delegates)
8. [Extension Points](#extension-points)
9. [Networking](#networking)
10. [Known Limitations](#known-limitations)

***

## Installation

1. Copy the `DamageCore` plugin folder into your project's `Plugins/` directory.
2. Enable the plugin in **Edit → Plugins → DamageCore**.
3. Ensure `DT_UniversalDamageFactors` exists at `/DamageCore/Data/DT_UniversalDamageFactors` (row struct: `FDealDamageRow`). The component logs an error on startup if the asset is missing.
4. Add `"DamageCore"` to your module's `PublicDependencyModuleNames` in `Build.cs`.

***

## Quick Start

### C++

```cpp
// In your Actor header
UPROPERTY(VisibleAnywhere)
UDamageableComponent* DamageableComponent;

// In the constructor
DamageableComponent = CreateDefaultSubobject<UDamageableComponent>(TEXT("DamageableComponent"));
```

### Blueprint

Add `DamageableComponent` to any Blueprint Actor from the **Components** panel. No C++ required.

### Dealing damage

```cpp
// From the instigator's DamageableComponent
DamageableComponent->TryToDealDamage(TargetActor, DamageTag);
```

### Restoring health

```cpp
DamageableComponent->TryToHeal(TargetActor, RestorationTag);
```

***

## Configuration

All parameters are exposed as `EditAnywhere / BlueprintReadWrite` UPROPERTY and configurable per instance in the Details panel.

### Health

| Property | Type | Default | Description |
|---|---|---|---|
| `MaxHealth` | `float` | `100.0` | Maximum health. Health is initialized to this value on `BeginPlay`. |
| `bDebug` | `bool` | `false` | Enables verbose `LogDamageCore` logging for all damage and restoration operations. |

### Damage Factors

| Property | Type | Description |
|---|---|---|
| `DamageFactors` | `TMap<FGameplayTag, FDealDamageInfo>` | Per-tag damage configuration. Each entry maps a Gameplay Tag to a list of `FDamageValue` entries keyed by target actor class. |
| `DamageReceptionOverrides` | `TMap<FGameplayTag, FDealDamageInfo>` | Declarative incoming damage overrides. If the incoming tag + instigator class matches an entry, it bypasses `ModifyIncomingDamage` entirely. |

**`FDamageValue` fields:**

| Field | Type | Description |
|---|---|---|
| `ActorClass` | `TSubclassOf<AActor>` | Target class this value applies to (subclasses match). |
| `DamageValue` | `float` | Amount of damage. |
| `bIsPercentDamage` | `bool` | If true, `DamageValue` is treated as a percentage of `MaxHealth`. |
| `bIsMortalDamage` | `bool` | If true, this damage type can kill (reduce health to 0). |

> **Fallback:** If no match is found in `DamageFactors`, the component automatically queries `DT_UniversalDamageFactors`. Rows with `bIsClassAgnostic = true` match any target class.

### Restoration

| Property | Type | Description |
|---|---|---|
| `Restorations` | `TArray<FRestoreValue>` | List of restoration configurations, each identified by a `RestorationTag`. |

**`FRestoreValue` fields:**

| Field | Type | Description |
|---|---|---|
| `RestorationTag` | `FGameplayTag` | Tag identifying this restore entry. |
| `HealthToAdd` | `float` | Amount of health to restore. |
| `bIsPercentValue` | `bool` | If true, `HealthToAdd` is a percentage of `MaxHealth`. |
| `bIsTimer` | `bool` | If true, triggers a delayed single regen tick instead of an instant heal. |
| `RestoreTimerDelay` | `float` | Delay in seconds before the regen tick fires (only used when `bIsTimer = true`). |

> Entries with `RestorationTag` matching `RESTORATION_AUTO` are triggered **automatically** whenever the actor takes damage and still has health remaining.

### Delegates

| Property | Type | Default | Description |
|---|---|---|---|
| `bNotifyInstigatorOnDamageDealt` | `bool` | `true` | If true, `OnDamageDealtDelegate` is also broadcast on the instigator's owning client when damage is dealt. |

***

## Damage System

### Pipeline — `TryToDealDamage`

Every damage request goes through a strict, ordered pipeline. All steps run **server-side only**.

```
Client call
    └─ bNotifyServerIfClient == true → ServerTryToDealDamage RPC → server
    └─ bNotifyServerIfClient == false → no-op, returns 0

Server:
  1. CanDealDamage()              — can the instigator act?
  2. CanDealDamageToActor()       — is the target valid and damageable?
  3. FindDamageValue()            — anti-cheat: is the instigator registered for this tag + class?
  4. CanTakeDamage() [2nd check]  — race-condition guard on the target
  5. ModifyOutgoingDamage()       — instigator modifier (buffs, crits)
  6. ModifyIncomingDamage()       — target modifier (armor, immunities)
  7. ApplyDamage()                — commit + dispatch delegates
```

```cpp
float TryToDealDamage(
    AActor* ActorToDamage,
    const FGameplayTag& DamageTag,
    const FHitResult HitResult = FHitResult(),
    const bool bNotifyServerIfClient = false
);
// Returns: effective health reduction, or 0 if the pipeline was aborted at any step.
```

### Raw Damage — `ApplyRawDamage`

Bypasses tag lookup, anti-cheat, and Modify hooks. For fall damage, environmental hazards, or any source without a `DamageableComponent`.

```cpp
float ApplyRawDamage(const FDamageValue& DamageValue, AActor* Instigator = nullptr);
// Authority only.
// Returns: effective health reduction after clamping.
```

### Anti-Cheat

`FindDamageValue` ensures that an instigator can only deal damage types it has explicitly declared in `DamageFactors` (or `DT_UniversalDamageFactors`). If no matching entry is found for the `(DamageTag, TargetClass)` pair, the pipeline aborts and returns 0. This prevents clients from triggering arbitrary damage amounts.

***

## Restoration System

### Tag Routing

`TryToHeal` routes the restoration request based on the tag:

| Tag type | Behavior |
|---|---|
| Matches `RESTORATION_FIXED` or any child | Bypasses `Restorations` array. Maps to a fixed amount: `QUARTER` = MaxHealth/4, `HALF` = MaxHealth/2, `FULL` = MaxHealth. |
| Any other tag | Resolved from the instigator's `Restorations` array via `GetRestoreValueFromTag`. |

```cpp
float TryToHeal(
    AActor* ActorToHeal,
    const FGameplayTag& RestorationTag,
    const bool bNotifyServerIfClient = false
);
// Returns: health effectively added, or -1 if a regen timer was started instead.
```

### Auto-Regeneration

Add a `FRestoreValue` entry to `Restorations` with `RestorationTag = RESTORATION_AUTO`. It will fire automatically every time this actor takes damage and survives. The regen is a **single delayed tick** (not a continuous loop) — configure `RestoreTimerDelay` to control when it fires after taking damage.

> Only one regen timer runs at a time. Taking damage while a timer is pending cancels the current timer and starts a fresh one.

***

## Prediction

`PredictHealthAfterDamage` mirrors the full `ModifyOutgoing → ModifyIncoming` pipeline **without writing any value**. Useful for UI previews, ability targeting, or AI decision-making.

```cpp
float PredictHealthAfterDamage(
    AActor* ActorToDamage,
    const FGameplayTag& DamageTag,
    const FHitResult HitResult = FHitResult(),
    const bool bClampedResult = true,   // clamp to [0, MaxHealth]
    const bool bReturnPercent = false   // return as % of MaxHealth (0–100)
);
// Returns: predicted health value.
// Returns target's current Health if the pipeline would abort (missing tag, blocked by Modify).
// Returns 0 if ActorToDamage is null.
```

***

## Delegates

All delegates are `BlueprintAssignable`.

| Delegate | Broadcast scope | Signature | When |
|---|---|---|---|
| `OnHealthChangedDelegate` | Server + all clients (NetMulticast) | `(float NewHealth, bool bIncreased, FLastReceivedDamage, FLastReceivedHealth)` | Every Health change |
| `OnDamageTakenDelegate` | Owning client only | `(float DamageTaken, FHitResult, AActor* InstigatedBy)` | This actor takes damage |
| `OnDamageDealtDelegate` | Instigator's client only | `(float DamageDealt, FHitResult, AActor* DamagedActor, float DamagedActorNewLife)` | This actor deals damage |
| `OnHitDelegate` | Server + all clients (NetMulticast) | `(float Damage, FHitResult, AActor* InstigatedBy)` | This actor is hit |
| `OnHealthDepleted` | Server + all clients (NetMulticast) | `(FLastReceivedDamage)` | Health reaches 0 |

### Usage example (Blueprint)

Bind `OnHealthDepleted` to trigger a ragdoll or respawn flow. Use `LastReceivedDamage.bIsMortalDamage` to decide between a downed state and an instant kill.

### Usage example (C++)

```cpp
DamageableComponent->OnHealthDepleted.AddDynamic(this, &AMyCharacter::OnDied);
DamageableComponent->OnDamageTakenDelegate.AddDynamic(this, &AMyCharacter::PlayHitReaction);
```

***

## Extension Points

All five gate functions and both Modify hooks are `BlueprintNativeEvent`, overridable in Blueprint or C++ **without subclassing the actor**.

### Gate conditions

| Function | Default | Override use-case |
|---|---|---|
| `CanTakeDamage()` | `CanBeDamaged() && Health > 0` | Invincibility frames, shields |
| `CanDealDamage()` | `true` | Stunned, disarmed, silenced |
| `CanDealDamageToActor(AActor*)` | Target valid + `CanTakeDamage()` | Team check, range, line-of-sight |
| `CanBeHealed()` | `true` | Cursed / anti-heal effects *(not yet enforced — see Known Limitations)* |
| `CanDie()` | `true` | Immortal phase, last-stand mechanic (clamps Health to 1 minimum) |

### Damage modifiers

```cpp
// Override to apply armor, resistances, or immunities.
// Return an invalid FDamageValue (Value <= 0) to block all damage.
UFUNCTION(BlueprintNativeEvent)
FDamageValue ModifyIncomingDamage(
    const FGameplayTag& DamageType,
    FDamageValue DamageValue,
    const FHitResult& HitResult,
    AActor* Instigator
);

// Override to apply buffs, critical hit multipliers, or damage bonuses.
UFUNCTION(BlueprintNativeEvent)
FDamageValue ModifyOutgoingDamage(
    const FGameplayTag& DamageType,
    FDamageValue DamageValue,
    const FHitResult& HitResult,
    AActor* ActorToDamage
);
```

> Declarative overrides via `DamageReceptionOverrides` take priority over `ModifyIncomingDamage` — no code needed for simple per-class damage adjustments.

***

## Networking

| Concern | Implementation |
|---|---|
| Health replication | `DOREPLIFETIME` to all connections |
| Combat metadata | `DOREPLIFETIME_CONDITION_NOTIFY` owner-only, on change |
| All Health writes | Authority-only (`HasAuthority()` guard on every entry point) |
| `TryToDealDamage` from client | Forwarded via `ServerTryToDealDamage` (Reliable RPC) if `bNotifyServerIfClient = true` |
| `TryToHeal` from client | Forwarded via `ServerTryToRestoreHealth` (Reliable RPC) if `bNotifyServerIfClient = true` |
| Damage notification | `ClientNotifyDamageTaken` (target's client), `ClientNotifyDamageDealt` (instigator's client) |
| Hit / Health change | `NetMulticastNotifyHit`, `NetMulticastDispatchHealthChangedDelegates` (all connections) |

> Set `bNotifyServerIfClient = false` when your game logic already runs on the server (e.g. server-side hit detection) to avoid duplicate damage events.

***

## Known Limitations

- **`CanBeHealed()` is not enforced** — `TryToHeal` and `ApplyHealing` do not currently check this gate. Planned for a future update. Override at your own risk in the meantime.
- **Single regen tick** — the auto-regen timer fires once per damage event (not a continuous loop). This is intentional. For continuous regeneration, use a repeating timer in your actor and call `TryToHeal` manually each tick.
- **`DamageReceptionOverrides` uses exact instigator class match** — subclasses of the instigator are not matched. Keep this in mind when designing class hierarchies.

***

## License

MIT — free to use in commercial and personal projects. Attribution appreciated.
