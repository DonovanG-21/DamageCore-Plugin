#include "Data/HealthSystemTypes.h"

#pragma region HEALTH

#pragma region HEALTH DAMAGE TAGS

	// BASE
	UE_DEFINE_GAMEPLAY_TAG(DAMAGE_DEFAULT, "DamageCore.Damage");
	UE_DEFINE_GAMEPLAY_TAG(DAMAGE_ONESHOT, "DamageCore.Damage.OneShot");
	UE_DEFINE_GAMEPLAY_TAG(DAMAGE_RAW, "DamageCore.Damage.Raw");

#pragma endregion

#pragma region HEALTH REGENERATION TAGS

// FIXED
UE_DEFINE_GAMEPLAY_TAG(RESTORATION_DEFAULT, "DamageCore.Restoration")

UE_DEFINE_GAMEPLAY_TAG(RESTORATION_FIXED, "DamageCore.Restoration.Fixed")
UE_DEFINE_GAMEPLAY_TAG(RESTORATION_FIXED_QUARTER, "DamageCore.Restoration.Fixed.Quarter")
UE_DEFINE_GAMEPLAY_TAG(RESTORATION_FIXED_HALF, "DamageCore.Restoration.Fixed.Half")
UE_DEFINE_GAMEPLAY_TAG(RESTORATION_FIXED_FULL, "DamageCore.Restoration.Fixed.Full")

UE_DEFINE_GAMEPLAY_TAG(RESTORATION_AUTO, "DamageCore.Restoration.Auto")

#pragma endregion

#pragma endregion