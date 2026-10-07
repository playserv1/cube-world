// The lab's attributes, Lyra-style: Health and MaxHealth, Armor the blast's execution captures from the target (live),
// BlastPower it captures from the thrower (a snapshot taken at the throw), and Damage, a meta attribute the execution
// writes and PostGameplayEffectExecute turns into lost health.
#pragma once

#include "CoreMinimal.h"
#include "AttributeSet.h"
#include "AbilitySystemComponent.h"
#include "GasLabAttributeSet.generated.h"

UCLASS()
class CUBEWORLD_API UGasLabAttributeSet : public UAttributeSet
{
	GENERATED_BODY()

public:
	UGasLabAttributeSet();

	ATTRIBUTE_ACCESSORS_BASIC(UGasLabAttributeSet, Health)
	ATTRIBUTE_ACCESSORS_BASIC(UGasLabAttributeSet, MaxHealth)
	ATTRIBUTE_ACCESSORS_BASIC(UGasLabAttributeSet, Armor)
	ATTRIBUTE_ACCESSORS_BASIC(UGasLabAttributeSet, BlastPower)
	ATTRIBUTE_ACCESSORS_BASIC(UGasLabAttributeSet, Damage)

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue) override;
	virtual void PostGameplayEffectExecute(const struct FGameplayEffectModCallbackData& Data) override;

	UPROPERTY(ReplicatedUsing = OnRep_Health) FGameplayAttributeData Health;
	UPROPERTY(ReplicatedUsing = OnRep_MaxHealth) FGameplayAttributeData MaxHealth;
	UPROPERTY(ReplicatedUsing = OnRep_Armor) FGameplayAttributeData Armor;
	UPROPERTY(ReplicatedUsing = OnRep_BlastPower) FGameplayAttributeData BlastPower;
	/** Meta: written by the blast's execution and the burning ticks, consumed at once, never replicated. */
	UPROPERTY() FGameplayAttributeData Damage;

protected:
	UFUNCTION() void OnRep_Health(const FGameplayAttributeData& Old);
	UFUNCTION() void OnRep_MaxHealth(const FGameplayAttributeData& Old);
	UFUNCTION() void OnRep_Armor(const FGameplayAttributeData& Old);
	UFUNCTION() void OnRep_BlastPower(const FGameplayAttributeData& Old);
};
