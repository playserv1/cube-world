// Throw a creeper: a locally predicted ability with a cooldown effect. The server makes the two specs (the blast with
// its SetByCaller damage and radius, the burning with its damage per tick) from the thrower's ability system, which
// snapshots the thrower's BlastPower into the blast, and hands them to the creeper it spawns.
#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "GasLabAbility.generated.h"

UCLASS()
class CUBEWORLD_API UGasLabAbility_Throw : public UGameplayAbility
{
	GENERATED_BODY()

public:
	UGasLabAbility_Throw();

	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
	/** The stock one allocates its context from the globals class; this one asks the ability system (the lab's context type). */
	virtual FGameplayEffectContextHandle MakeEffectContext(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo) const override;

	static constexpr float BlastDamage = 40.f;
	static constexpr float BurnDamage = 4.f;
	static constexpr float FlightSeconds = 1.1f;
	static constexpr float Range = 900.f;
};
