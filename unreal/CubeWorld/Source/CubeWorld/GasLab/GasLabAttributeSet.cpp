#include "GasLab/GasLabAttributeSet.h"
#include "GasLab/GasLabAbilitySystemComponent.h"
#include "GameplayEffectExtension.h"
#include "Net/UnrealNetwork.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GasLabAttributeSet)

UGasLabAttributeSet::UGasLabAttributeSet()
{
	InitHealth(100.f);
	InitMaxHealth(100.f);
	InitArmor(0.f);
	InitBlastPower(1.f);
	InitDamage(0.f);
}

void UGasLabAttributeSet::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME_CONDITION_NOTIFY(UGasLabAttributeSet, Health, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UGasLabAttributeSet, MaxHealth, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UGasLabAttributeSet, Armor, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UGasLabAttributeSet, BlastPower, COND_None, REPNOTIFY_Always);
}

void UGasLabAttributeSet::PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue)
{
	Super::PreAttributeChange(Attribute, NewValue);
	if (Attribute == GetHealthAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.f, GetMaxHealth());
	}
}

void UGasLabAttributeSet::PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data)
{
	Super::PostGameplayEffectExecute(Data);
	if (Data.EvaluatedData.Attribute != GetDamageAttribute())
	{
		return;
	}
	const float Dealt = GetDamage();
	SetDamage(0.f);
	if (Dealt <= 0.f)
	{
		return;
	}
	const float Before = GetHealth();
	SetHealth(FMath::Clamp(Before - Dealt, 0.f, GetMaxHealth()));
	if (UGasLabAbilitySystemComponent* ASC = Cast<UGasLabAbilitySystemComponent>(GetOwningAbilitySystemComponent()))
	{
		ASC->NoteDamage(Data.EffectSpec, Before - GetHealth(), Before > 0.f && GetHealth() <= 0.f);
	}
}

void UGasLabAttributeSet::OnRep_Health(const FGameplayAttributeData& Old) { GAMEPLAYATTRIBUTE_REPNOTIFY(UGasLabAttributeSet, Health, Old); }
void UGasLabAttributeSet::OnRep_MaxHealth(const FGameplayAttributeData& Old) { GAMEPLAYATTRIBUTE_REPNOTIFY(UGasLabAttributeSet, MaxHealth, Old); }
void UGasLabAttributeSet::OnRep_Armor(const FGameplayAttributeData& Old) { GAMEPLAYATTRIBUTE_REPNOTIFY(UGasLabAttributeSet, Armor, Old); }
void UGasLabAttributeSet::OnRep_BlastPower(const FGameplayAttributeData& Old) { GAMEPLAYATTRIBUTE_REPNOTIFY(UGasLabAttributeSet, BlastPower, Old); }
