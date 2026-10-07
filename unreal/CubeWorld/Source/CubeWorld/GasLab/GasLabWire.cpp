#include "GasLab/GasLabWire.h"
#include "GasLab/GasLabEffectContext.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "GameplayEffect.h"
#include "GameplayEffectAggregator.h"
#include "GameplayTagsManager.h"

namespace GasLabWire
{
	FString AttributeName(const FGameplayAttribute& Attribute)
	{
		const UStruct* Owner = Attribute.GetAttributeSetClass();
		return Owner ? FString::Printf(TEXT("%s:%s"), *Owner->GetPathName(), *Attribute.GetName()) : FString();
	}

	FGameplayAttribute FindAttribute(const FString& Name)
	{
		FString ClassPath, Property;
		if (!Name.Split(TEXT(":"), &ClassPath, &Property)) return FGameplayAttribute();
		const UClass* Class = FindObject<UClass>(nullptr, *ClassPath);
		if (!Class) return FGameplayAttribute();
		FProperty* Prop = FindFProperty<FProperty>(Class, *Property);
		return Prop ? FGameplayAttribute(Prop) : FGameplayAttribute();
	}

	TArray<FName> TagNames(const FGameplayTagContainer& Tags)
	{
		TArray<FName> Out;
		for (const FGameplayTag& Tag : Tags) Out.Add(Tag.GetTagName());
		return Out;
	}

	FGameplayTagContainer Tags(const TArray<FName>& Names)
	{
		FGameplayTagContainer Out;
		for (const FName& Name : Names)
		{
			const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(Name, false);
			if (Tag.IsValid()) Out.AddTag(Tag);
		}
		return Out;
	}
}

namespace
{
	void CaptureDefinitions(const UGameplayEffect& Def, TArray<FGameplayEffectAttributeCaptureDefinition>& Out)
	{
		// Each of GAS's getters empties the array it is given: collect them one by one.
		TArray<FGameplayEffectAttributeCaptureDefinition> One;
		for (const FGameplayModifierInfo& Mod : Def.Modifiers) { Mod.ModifierMagnitude.GetAttributeCaptureDefinitions(One); Out.Append(One); }
		for (const FGameplayEffectExecutionDefinition& Execution : Def.Executions) { Execution.GetAttributeCaptureDefinitions(One); Out.Append(One); }
		Def.DurationMagnitude.GetAttributeCaptureDefinitions(One);
		Out.Append(One);
	}

	void SerializeTagPairs(FArchive& Ar, TArray<TPair<FName, float>>& Pairs)
	{
		int32 Num = Pairs.Num();
		Ar << Num;
		if (Ar.IsLoading()) Pairs.SetNum(FMath::Clamp(Num, 0, 256));
		for (TPair<FName, float>& P : Pairs) { Ar << P.Key; Ar << P.Value; }
	}
}

FGasLabWireSpec FGasLabWireSpec::From(const FGameplayEffectSpec& Spec)
{
	FGasLabWireSpec W;
	if (!Spec.Def) return W;
	W.Def = Spec.Def->GetClass()->GetPathName();
	W.Level = Spec.GetLevel();
	W.StackCount = Spec.GetStackCount();
	for (const TPair<FGameplayTag, float>& P : Spec.SetByCallerTagMagnitudes) W.SetByCallerTags.Add({ P.Key.GetTagName(), P.Value });
	for (const TPair<FName, float>& P : Spec.SetByCallerNameMagnitudes) W.SetByCallerNames.Add({ P.Key, P.Value });
	W.DynamicGrantedTags = GasLabWire::TagNames(Spec.DynamicGrantedTags);
	W.DynamicAssetTags = GasLabWire::TagNames(Spec.GetDynamicAssetTags());
	W.SourceActorTags = GasLabWire::TagNames(Spec.CapturedSourceTags.GetActorTags());
	W.SourceSpecTags = GasLabWire::TagNames(Spec.CapturedSourceTags.GetSpecTags());

	const FGameplayEffectContextHandle& Context = Spec.GetEffectContext();
	if (const FGasLabEffectContext* Lab = FGasLabEffectContext::From(Context))
	{
		W.InstigatorId = Lab->InstigatorId;
		W.OriginRegion = Lab->OriginRegion;
		W.EventAt = Lab->EventAt;
	}
	if (Context.IsValid() && Context.HasOrigin())
	{
		W.bHasOrigin = true;
		W.Origin = Context.GetOrigin();
	}

	// Every source capture travels as numbers. A snapshot is exact; a live one is frozen at this moment.
	TArray<FGameplayEffectAttributeCaptureDefinition> Defs;
	CaptureDefinitions(*Spec.Def, Defs);
	FAggregatorEvaluateParameters Eval;
	Eval.SourceTags = Spec.CapturedSourceTags.GetAggregatedTags();
	Eval.TargetTags = Spec.CapturedTargetTags.GetAggregatedTags();
	TSet<FGameplayEffectAttributeCaptureDefinition> Seen;
	for (const FGameplayEffectAttributeCaptureDefinition& Def : Defs)
	{
		if (Def.AttributeSource != EGameplayEffectAttributeCaptureSource::Source || Seen.Contains(Def)) continue;
		Seen.Add(Def);
		const FGameplayEffectAttributeCaptureSpec* Captured = Spec.CapturedRelevantAttributes.FindCaptureSpecByDefinition(Def, true);
		if (!Captured) continue;
		FCapture C;
		C.Attribute = GasLabWire::AttributeName(Def.AttributeToCapture);
		C.bSnapshot = Def.bSnapshot;
		Captured->AttemptCalculateAttributeBaseValue(C.Base);
		Captured->AttemptCalculateAttributeMagnitude(Eval, C.Final);
		W.SourceCaptures.Add(C);
	}
	UE_LOG(LogTemp, Log, TEXT("GASLAB wire def=%s defs=%d captures=%d"), *W.Def, Defs.Num(), W.SourceCaptures.Num());
	return W;
}

TSharedPtr<FGameplayEffectSpec> FGasLabWireSpec::Build(UAbilitySystemComponent* Instigator) const
{
	UClass* Class = FindObject<UClass>(nullptr, *Def);
	if (!Class) Class = LoadObject<UClass>(nullptr, *Def);
	if (!Class || !Class->IsChildOf(UGameplayEffect::StaticClass())) return nullptr;
	const UGameplayEffect* GE = Class->GetDefaultObject<UGameplayEffect>();

	FGameplayEffectContextHandle Context(new FGasLabEffectContext());
	if (Instigator) Context.AddInstigator(Instigator->GetOwnerActor(), Instigator->GetAvatarActor());
	if (FGasLabEffectContext* Lab = FGasLabEffectContext::From(Context))
	{
		Lab->InstigatorId = InstigatorId;
		Lab->OriginRegion = OriginRegion;
		Lab->EventAt = EventAt;
	}
	if (bHasOrigin) Context.AddOrigin(Origin);

	TSharedPtr<FGameplayEffectSpec> Spec = MakeShared<FGameplayEffectSpec>(GE, Context, Level);
	for (const TPair<FName, float>& P : SetByCallerTags)
	{
		const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(P.Key, false);
		if (Tag.IsValid()) Spec->SetSetByCallerMagnitude(Tag, P.Value);
	}
	for (const TPair<FName, float>& P : SetByCallerNames) Spec->SetSetByCallerMagnitude(P.Key, P.Value);
	Spec->SetStackCount(StackCount);
	if (Duration >= 0.f) Spec->SetDuration(Duration, true);
	Spec->DynamicGrantedTags.AppendTags(GasLabWire::Tags(DynamicGrantedTags));
	Spec->AppendDynamicAssetTags(GasLabWire::Tags(DynamicAssetTags));
	// The constructor took the tags of whoever instigates here; the effect was made with the thrower's.
	Spec->CapturedSourceTags.GetActorTags() = GasLabWire::Tags(SourceActorTags);
	Spec->CapturedSourceTags.GetSpecTags() = GasLabWire::Tags(SourceSpecTags);

	for (const FCapture& C : SourceCaptures)
	{
		const FGameplayAttribute Attribute = GasLabWire::FindAttribute(C.Attribute);
		if (!Attribute.IsValid())
		{
			UE_LOG(LogTemp, Warning, TEXT("GASLAB wire attribute %s not found"), *C.Attribute);
			continue;
		}
		const FGameplayEffectAttributeCaptureDefinition CaptureDef(Attribute, EGameplayEffectAttributeCaptureSource::Source, C.bSnapshot);
		const FGameplayEffectAttributeCaptureSpec* Existing = Spec->CapturedRelevantAttributes.FindCaptureSpecByDefinition(CaptureDef, false);
		if (!Existing || Existing->HasValidCapture())
		{
			UE_LOG(LogTemp, Warning, TEXT("GASLAB wire capture %s: %s"), *C.Attribute, Existing ? TEXT("already captured") : TEXT("no capture spec"));
			continue;
		}
		// The thrower's value as a one-off aggregator: its base, and one mod that makes up the rest.
		FAggregator* Aggregator = new FAggregator(C.Base);
		if (!FMath::IsNearlyEqual(C.Final, C.Base))
		{
			Aggregator->AddAggregatorMod(C.Final - C.Base, EGameplayModOp::Additive, EGameplayModEvaluationChannel::Channel0, nullptr, nullptr, false);
		}
		const_cast<FGameplayEffectAttributeCaptureSpec*>(Existing)->SwapAggregator(FAggregatorRef(), FAggregatorRef(Aggregator));
	}
	return Spec;
}

FArchive& operator<<(FArchive& Ar, FGasLabWireSpec& S)
{
	Ar << S.Def << S.Level << S.StackCount << S.Duration;
	SerializeTagPairs(Ar, S.SetByCallerTags);
	SerializeTagPairs(Ar, S.SetByCallerNames);
	Ar << S.DynamicGrantedTags << S.DynamicAssetTags << S.SourceActorTags << S.SourceSpecTags;
	Ar << S.InstigatorId << S.OriginRegion << S.EventAt << S.bHasOrigin << S.Origin;
	int32 Num = S.SourceCaptures.Num();
	Ar << Num;
	if (Ar.IsLoading()) S.SourceCaptures.SetNum(FMath::Clamp(Num, 0, 64));
	for (FGasLabWireSpec::FCapture& C : S.SourceCaptures) Ar << C.Attribute << C.bSnapshot << C.Base << C.Final;
	return Ar;
}

FArchive& operator<<(FArchive& Ar, FGasLabWireActive& A)
{
	Ar << A.Spec << A.Duration << A.Elapsed << A.StartShared << A.PeriodRemaining;
	return Ar;
}

FArchive& operator<<(FArchive& Ar, FGasLabWirePlayer& P)
{
	Ar << P.Id << P.Name << P.bBot << P.FromRegion << P.Location << P.Velocity << P.Yaw;
	int32 Num = P.Bases.Num();
	Ar << Num;
	if (Ar.IsLoading()) P.Bases.SetNum(FMath::Clamp(Num, 0, 256));
	for (TPair<FString, float>& B : P.Bases) Ar << B.Key << B.Value;
	Ar << P.Effects << P.SentAt << P.DamageDealt << P.Kills;
	return Ar;
}

FArchive& operator<<(FArchive& Ar, FGasLabWireGhost& G)
{
	Ar << G.Id << G.Name << G.OwnerRegion << G.Location << G.Velocity << G.Yaw << G.Health << G.MaxHealth << G.Tags << G.Cues << G.SentAt;
	return Ar;
}

FArchive& operator<<(FArchive& Ar, FGasLabWireCreeper& C)
{
	Ar << C.Id << C.ThrowerId << C.OwnerRegion << C.Origin << C.Velocity << C.LaunchAt << C.bExploded << C.ExplodedAt << C.ExplodedAtTime;
	Ar << C.bCarriesEffects << C.Radius;
	if (C.bCarriesEffects) Ar << C.Blast << C.Burn;
	return Ar;
}

FArchive& operator<<(FArchive& Ar, FGasLabWireCredit& C)
{
	Ar << C.InstigatorId << C.TargetId << C.Damage << C.bKilled << C.At;
	return Ar;
}
