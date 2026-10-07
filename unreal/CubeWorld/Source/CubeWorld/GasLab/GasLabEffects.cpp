#include "GasLab/GasLabEffects.h"
#include "GasLab/GasLabAttributeSet.h"
#include "GasLab/GasLabTags.h"
#include "AbilitySystemComponent.h"
#include "GameplayEffectComponents/AssetTagsGameplayEffectComponent.h"
#include "GameplayEffectComponents/TargetTagsGameplayEffectComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GasLabEffects)

namespace
{
	struct FBlastCaptures
	{
		DECLARE_ATTRIBUTE_CAPTUREDEF(BlastPower);
		DECLARE_ATTRIBUTE_CAPTUREDEF(Armor);

		FBlastCaptures()
		{
			// The thrower's power as it was at the throw; the target's armor as it is when the blast lands.
			DEFINE_ATTRIBUTE_CAPTUREDEF(UGasLabAttributeSet, BlastPower, Source, true);
			DEFINE_ATTRIBUTE_CAPTUREDEF(UGasLabAttributeSet, Armor, Target, false);
		}
	};

	const FBlastCaptures& Captures()
	{
		static FBlastCaptures Instance;
		return Instance;
	}

	FInheritedTagContainer Tags(const FGameplayTag& Tag)
	{
		FInheritedTagContainer Container;
		Container.Added.AddTag(Tag);
		return Container;
	}
}

UGasLabBlastExecution::UGasLabBlastExecution()
{
	RelevantAttributesToCapture.Add(Captures().BlastPowerDef);
	RelevantAttributesToCapture.Add(Captures().ArmorDef);
}

float UGasLabBlastExecution::Damage(float Base, float Power, float Radius, float Distance, float Armor)
{
	if (Radius <= 0.f || Distance > Radius)
	{
		return 0.f;
	}
	// Full damage at the centre, half at the rim; armor takes its share as Lyra's damage execution does.
	const float Falloff = 1.f - 0.5f * FMath::Clamp(Distance / Radius, 0.f, 1.f);
	return Base * FMath::Max(Power, 0.f) * Falloff * 100.f / (100.f + FMath::Max(Armor, 0.f));
}

void UGasLabBlastExecution::Execute_Implementation(const FGameplayEffectCustomExecutionParameters& ExecutionParams, FGameplayEffectCustomExecutionOutput& OutExecutionOutput) const
{
	const FGameplayEffectSpec& Spec = ExecutionParams.GetOwningSpec();
	FAggregatorEvaluateParameters Eval;
	Eval.SourceTags = Spec.CapturedSourceTags.GetAggregatedTags();
	Eval.TargetTags = Spec.CapturedTargetTags.GetAggregatedTags();

	float Power = 1.f, Armor = 0.f;
	ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(Captures().BlastPowerDef, Eval, Power);
	ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(Captures().ArmorDef, Eval, Armor);

	const float Base = Spec.GetSetByCallerMagnitude(GasLabTags::Data_Damage, false, 0.f);
	const float Radius = Spec.GetSetByCallerMagnitude(GasLabTags::Data_Radius, false, 0.f);

	// The distance is taken where the target stands on its own server, from the centre the context carries.
	float Distance = 0.f;
	const FGameplayEffectContextHandle& Context = Spec.GetEffectContext();
	const UAbilitySystemComponent* Target = ExecutionParams.GetTargetAbilitySystemComponent();
	if (Context.HasOrigin() && Target && Target->GetAvatarActor_Direct())
	{
		Distance = FVector::Dist(Context.GetOrigin(), Target->GetAvatarActor_Direct()->GetActorLocation());
	}

	const float Dealt = Damage(Base, Power, Radius, Distance, Armor);
	if (Dealt > 0.f)
	{
		OutExecutionOutput.AddOutputModifier(FGameplayModifierEvaluatedData(UGasLabAttributeSet::GetDamageAttribute(), EGameplayModOp::Additive, Dealt));
	}
}

UGasLabGE_Blast::UGasLabGE_Blast(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DurationPolicy = EGameplayEffectDurationType::Instant;
	FGameplayEffectExecutionDefinition Execution;
	Execution.CalculationClass = UGasLabBlastExecution::StaticClass();
	Executions.Add(Execution);
	GameplayCues.Add(FGameplayEffectCue(GasLabTags::GameplayCue_Creeper_Blast, 0.f, 0.f));
	AssetTags = CreateDefaultSubobject<UAssetTagsGameplayEffectComponent>(TEXT("AssetTags"));
	GEComponents.Add(AssetTags);
}

void UGasLabGE_Blast::PostInitProperties()
{
	Super::PostInitProperties();
	if (AssetTags) AssetTags->SetAndApplyAssetTagChanges(Tags(GasLabTags::Effect_Blast));
}

UGasLabGE_Burning::UGasLabGE_Burning(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;
	DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(Seconds));
	Period = FScalableFloat(Every);
	bExecutePeriodicEffectOnApplication = false;

	FSetByCallerFloat PerTick;
	PerTick.DataTag = GasLabTags::Data_BurnDamage;
	FGameplayModifierInfo Burn;
	Burn.Attribute = UGasLabAttributeSet::GetDamageAttribute();
	Burn.ModifierOp = EGameplayModOp::Additive;
	Burn.ModifierMagnitude = FGameplayEffectModifierMagnitude(PerTick);
	Modifiers.Add(Burn);

	// SetStackingType is not exported from the plugin in 5.8; the property it sets is still public, if deprecated.
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	StackingType = EGameplayEffectStackingType::AggregateByTarget;
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
	StackLimitCount = MaxStacks;
	StackDurationRefreshPolicy = EGameplayEffectStackingDurationPolicy::RefreshOnSuccessfulApplication;
	StackPeriodResetPolicy = EGameplayEffectStackingPeriodPolicy::NeverReset;
	StackExpirationPolicy = EGameplayEffectStackingExpirationPolicy::ClearEntireStack;

	GameplayCues.Add(FGameplayEffectCue(GasLabTags::GameplayCue_Burning, 0.f, 0.f));
	TargetTags = CreateDefaultSubobject<UTargetTagsGameplayEffectComponent>(TEXT("TargetTags"));
	GEComponents.Add(TargetTags);
	AssetTags = CreateDefaultSubobject<UAssetTagsGameplayEffectComponent>(TEXT("AssetTags"));
	GEComponents.Add(AssetTags);
}

void UGasLabGE_Burning::PostInitProperties()
{
	Super::PostInitProperties();
	if (TargetTags) TargetTags->SetAndApplyTargetTagChanges(Tags(GasLabTags::State_Burning));
	if (AssetTags) AssetTags->SetAndApplyAssetTagChanges(Tags(GasLabTags::Effect_Burning));
}

UGasLabGE_ThrowCooldown::UGasLabGE_ThrowCooldown(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	DurationPolicy = EGameplayEffectDurationType::HasDuration;
	DurationMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(Seconds));
	TargetTags = CreateDefaultSubobject<UTargetTagsGameplayEffectComponent>(TEXT("TargetTags"));
	GEComponents.Add(TargetTags);
}

void UGasLabGE_ThrowCooldown::PostInitProperties()
{
	Super::PostInitProperties();
	if (TargetTags) TargetTags->SetAndApplyTargetTagChanges(Tags(GasLabTags::Cooldown_Throw));
}
