// The creeper's effects, as a studio would set them up in GameplayEffect assets, written as C++ classes so every server
// resolves the same definition by class path:
//   - the blast: instant, its damage worked out by an execution that captures the thrower's BlastPower (a snapshot)
//     and the target's Armor (live), with a falloff from the blast's centre; the Blast cue on every target hit;
//   - burning: 6 s, a tick every second, stacking up to three per target, granting State.Burning and the Burning cue;
//   - the throw's cooldown: 4 s of Cooldown.Throw.
#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "GameplayEffectExecutionCalculation.h"
#include "GasLabEffects.generated.h"

class UTargetTagsGameplayEffectComponent;
class UAssetTagsGameplayEffectComponent;

UCLASS()
class CUBEWORLD_API UGasLabBlastExecution : public UGameplayEffectExecutionCalculation
{
	GENERATED_BODY()

public:
	UGasLabBlastExecution();
	virtual void Execute_Implementation(const FGameplayEffectCustomExecutionParameters& ExecutionParams, FGameplayEffectCustomExecutionOutput& OutExecutionOutput) const override;

	/** The damage a blast of base Damage, power Power and radius Radius does at Distance to a target with Armor. */
	static float Damage(float Base, float Power, float Radius, float Distance, float Armor);
};

UCLASS()
class CUBEWORLD_API UGasLabGE_Blast : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UGasLabGE_Blast(const FObjectInitializer& ObjectInitializer);
	virtual void PostInitProperties() override;

private:
	UPROPERTY() TObjectPtr<UAssetTagsGameplayEffectComponent> AssetTags;
};

UCLASS()
class CUBEWORLD_API UGasLabGE_Burning : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UGasLabGE_Burning(const FObjectInitializer& ObjectInitializer);
	virtual void PostInitProperties() override;

	static constexpr float Seconds = 6.f;
	static constexpr float Every = 1.f;
	static constexpr int32 MaxStacks = 3;

private:
	UPROPERTY() TObjectPtr<UTargetTagsGameplayEffectComponent> TargetTags;
	UPROPERTY() TObjectPtr<UAssetTagsGameplayEffectComponent> AssetTags;
};

UCLASS()
class CUBEWORLD_API UGasLabGE_ThrowCooldown : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UGasLabGE_ThrowCooldown(const FObjectInitializer& ObjectInitializer);
	virtual void PostInitProperties() override;

	static constexpr float Seconds = 4.f;

private:
	UPROPERTY() TObjectPtr<UTargetTagsGameplayEffectComponent> TargetTags;
};
