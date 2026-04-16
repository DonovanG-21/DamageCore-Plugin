// Fill out your copyright notice in the Description page of Project Settings.
#pragma once

#include "CoreMinimal.h"

#include "Engine/DataTable.h"
#include "GameplayTagContainer.h"
#include "NativeGameplayTags.h"
#include "GameFramework/Actor.h"

#include "HealthSystemTypes.generated.h"



#pragma region GAMEPLAY TAG

	#pragma region HEALTH DAMAGE TAGS

		DAMAGECORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(DAMAGE_DEFAULT);
		DAMAGECORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(DAMAGE_ONESHOT);

		DAMAGECORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(DAMAGE_RAW);

	#pragma endregion

	#pragma region HEALTH REGENERATION TAGS

		DAMAGECORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(RESTORATION_DEFAULT);

		DAMAGECORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(RESTORATION_FIXED);
		DAMAGECORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(RESTORATION_FIXED_QUARTER);
		DAMAGECORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(RESTORATION_FIXED_HALF);
		DAMAGECORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(RESTORATION_FIXED_FULL);

		DAMAGECORE_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(RESTORATION_AUTO);

	#pragma endregion

#pragma endregion

#pragma region STRUCTURES

// FRestoreValue
USTRUCT(BlueprintType)
struct FRestoreValue : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DamageCore")
		FGameplayTag RestorationTag = RESTORATION_DEFAULT;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DamageCore", meta = (UIMin= 1.f, UIMax=100.f))
		bool bIsPercentValue = true;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DamageCore", meta = (UIMin= 1.f, UIMax=100.f))
		float HealthToAdd = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DamageCore")
		bool bIsTimer = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DamageCore", meta = (UIMin= 0.1f, UIMax=300.f, EditCondition = "bIsTimer == true", EditConditionHides))
		float RestoreTimerDelay = 1.f;
	
	FRestoreValue()
	{
		HealthToAdd = 0.f;
	}
	
	FRestoreValue(const float NewHealthToAdd)
	{
		HealthToAdd = NewHealthToAdd;
	}
	
	FRestoreValue(const float NewHealthToAdd, const bool bNewIsPercentValue)
	{
		bIsPercentValue = bNewIsPercentValue;
		HealthToAdd = NewHealthToAdd;
	}
	
	FORCEINLINE bool IsValid() const { return HealthToAdd > 0.f; }
};

// FDamageValue
USTRUCT(BlueprintType)
struct FDamageValue
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DamageCore")
		TSubclassOf<AActor> ActorClass;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DamageCore")
		bool bIsPercentDamage;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DamageCore")
		float DamageValue;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DamageCore")
		bool bIsMortalDamage;
	
	FDamageValue()
	{
		bIsPercentDamage = true;
		DamageValue = 0.f;
		bIsMortalDamage = true;
	}

	FDamageValue(const float NewDamageValue)
	{
		bIsPercentDamage = true;
		DamageValue = NewDamageValue;
		bIsMortalDamage = true;
	}
	
	FDamageValue(const float NewDamageValue, const bool bNewIsPercentDamage)
	{
		bIsPercentDamage = bNewIsPercentDamage;
		DamageValue = NewDamageValue;
		bIsMortalDamage = true;
	}
	
	FDamageValue(const float NewDamageValue, const bool bNewIsPercentDamage, const bool bNewIsMortalDamage)
	{
		bIsPercentDamage = bNewIsPercentDamage;
		DamageValue = NewDamageValue;
		bIsMortalDamage = bNewIsMortalDamage;
	}
	
	FORCEINLINE bool IsValid() const { return DamageValue > 0.f; }
};

// FDealDamageInfo
USTRUCT(BlueprintType)
struct FDealDamageInfo
{
	GENERATED_BODY()

	FDealDamageInfo()
	{
		
	}
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DamageCore")
		TArray<FDamageValue> DamageValues;
};


USTRUCT(BlueprintType)
struct FDealDamageRow : public FTableRowBase
{
	GENERATED_BODY()
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DamageCore")
	FGameplayTag DamageTag;
	
	/**
	 * If true, damage is applied to any target class without verification. 
	 * It will take only the first row of DamageValues
	 * If false, the instigator must have a matching entry in DamageValues
	 * for the target's class before this damage can be applied.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DamageCore")
	bool bIsClassAgnostic = true;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DamageCore")
	TArray<FDamageValue> DamageValues;
	
	FDealDamageRow()
	{
		
	}
};


USTRUCT(BlueprintType)
struct FLastReceivedDamage
{
	GENERATED_BODY()
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DamageCore")
	TObjectPtr<AActor> Instigator;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DamageCore")
	FDamageValue DamageValue;
	
	FLastReceivedDamage()
	{
		Instigator = nullptr;
		DamageValue = FDamageValue();
	}
	
	FLastReceivedDamage(const FDamageValue& NewDamageValue, AActor* NewInstigator)
	{
		Instigator = NewInstigator;
		DamageValue = NewDamageValue;
	}
	
	bool IsValid() const { return DamageValue.IsValid(); }
};

USTRUCT(BlueprintType)
struct FLastReceivedHealth
{
	GENERATED_BODY()
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DamageCore")
	TObjectPtr<AActor> Instigator;
	
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "DamageCore")
	FRestoreValue RestoreValue;
	
	FLastReceivedHealth()
	{
		Instigator = nullptr;
		RestoreValue = FRestoreValue();
	}
	
	FLastReceivedHealth(const FRestoreValue& NewRestoreValue, AActor* NewInstigator)
	{
		Instigator = NewInstigator;
		RestoreValue = NewRestoreValue;
	}
	
	bool IsValid() const { return RestoreValue.IsValid(); }
};

#pragma endregion
