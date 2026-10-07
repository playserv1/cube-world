#include "GasLab/GasLabAbilitySystemComponent.h"
#include "GasLab/GasLabAttributeSet.h"
#include "GasLab/GasLabBorder.h"
#include "GasLab/GasLabClock.h"
#include "GasLab/GasLabEffectContext.h"
#include "TimerManager.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GasLabAbilitySystemComponent)

UGasLabAbilitySystemComponent::UGasLabAbilitySystemComponent()
{
	SetIsReplicatedByDefault(true);
	// Lyra's setting for players: effects to the owner only, cues and tags to everyone.
	ReplicationMode = EGameplayEffectReplicationMode::Mixed;
}

FActiveGameplayEffectHandle UGasLabAbilitySystemComponent::ApplyGameplayEffectSpecToSelf(const FGameplayEffectSpec& Spec, FPredictionKey PredictionKey)
{
	if ((bGhost || bLeaving) && !bApplyingHere)
	{
		if (UGasLabBorder* Border = UGasLabBorder::Get(this))
		{
			Border->ForwardEffect(*this, Spec);
		}
		// Nothing is active here: the effect lives on the server that owns the player.
		return FActiveGameplayEffectHandle();
	}
	return Super::ApplyGameplayEffectSpecToSelf(Spec, PredictionKey);
}

FActiveGameplayEffectHandle UGasLabAbilitySystemComponent::ApplyHere(const FGameplayEffectSpec& Spec)
{
	TGuardValue<bool> Guard(bApplyingHere, true);
	return Super::ApplyGameplayEffectSpecToSelf(Spec, FPredictionKey());
}

FGameplayEffectContextHandle UGasLabAbilitySystemComponent::MakeEffectContext() const
{
	// The lab's context type, made here rather than by the globals class: in an editor-run server GAS creates its
	// globals before the project's module is loaded, and AbilitySystemGlobalsClassName is not applied.
	FGameplayEffectContextHandle Handle(new FGasLabEffectContext());
	Handle.AddInstigator(GetOwnerActor(), GetAvatarActor_Direct());
	if (FGasLabEffectContext* Context = FGasLabEffectContext::From(Handle))
	{
		Context->InstigatorId = PlayerId;
		if (const UGasLabBorder* Border = UGasLabBorder::Get(this))
		{
			Context->OriginRegion = Border->GetRegion();
		}
	}
	return Handle;
}

void UGasLabAbilitySystemComponent::NoteDamage(const FGameplayEffectSpec& Spec, float Lost, bool bKilled)
{
	if (UGasLabBorder* Border = UGasLabBorder::Get(this))
	{
		Border->NoteDamage(*this, Spec, Lost, bKilled);
	}
}

void UGasLabAbilitySystemComponent::Credit(const FString& TargetId, float Damage, bool bKilled, bool bElsewhere)
{
	DamageDealt += Damage;
	if (bKilled) Kills++;
	OnDamageDealt.Broadcast(TargetId, Damage, bKilled, bElsewhere);
}

void UGasLabAbilitySystemComponent::CaptureState(FGasLabWirePlayer& Out) const
{
	for (const UAttributeSet* Set : GetSpawnedAttributes())
	{
		if (!Set) continue;
		for (TFieldIterator<FProperty> It(Set->GetClass()); It; ++It)
		{
			if (!FGameplayAttribute::IsGameplayAttributeDataProperty(*It)) continue;
			const FGameplayAttribute Attribute(*It);
			Out.Bases.Add({ GasLabWire::AttributeName(Attribute), GetNumericAttributeBase(Attribute) });
		}
	}
	const UWorld* World = GetWorld();
	const double WorldNow = World ? World->GetTimeSeconds() : 0.0;
	for (const FActiveGameplayEffect& Active : &ActiveGameplayEffects)
	{
		if (Active.IsPendingRemove) continue;
		FGasLabWireActive A;
		A.Spec = FGasLabWireSpec::From(Active.Spec);
		A.Spec.StackCount = Active.Spec.GetStackCount();
		A.Duration = Active.GetDuration();
		A.Elapsed = float(WorldNow - Active.StartWorldTime);
		A.StartShared = Active.StartServerWorldTime;
		A.PeriodRemaining = (Active.GetPeriod() > 0.f && World) ? World->GetTimerManager().GetTimerRemaining(Active.PeriodHandle) : -1.f;
		Out.Effects.Add(MoveTemp(A));
	}
	Out.DamageDealt = DamageDealt;
	Out.Kills = Kills;
}

void UGasLabAbilitySystemComponent::RestoreState(const FGasLabWirePlayer& State, FGasLabRestoreReport& Report)
{
	UWorld* World = GetWorld();
	if (!World) return;
	for (const TPair<FString, float>& Base : State.Bases)
	{
		const FGameplayAttribute Attribute = GasLabWire::FindAttribute(Base.Key);
		if (Attribute.IsValid() && HasAttributeSetForAttribute(Attribute)) SetNumericAttributeBase(Attribute, Base.Value);
	}
	DamageDealt = State.DamageDealt;
	Kills = State.Kills;

	// With one clock the time the state spent between the servers counts against the effects too.
	const bool bShared = GasLabClock::IsShared();
	const double NowShared = GasLabClock::Now();
	const float Transit = bShared ? float(FMath::Max(0.0, NowShared - State.SentAt)) : 0.f;
	Report.Transit = float(FMath::Max(0.0, NowShared - State.SentAt));
	FTimerManager& Timers = World->GetTimerManager();
	for (const FGasLabWireActive& A : State.Effects)
	{
		Report.Effects++;
		FGasLabWireSpec Wire = A.Spec;
		if (A.Duration > 0.f) Wire.Duration = A.Duration;
		TSharedPtr<FGameplayEffectSpec> Spec = Wire.Build(nullptr);
		if (!Spec) continue;
		const FActiveGameplayEffectHandle Handle = ApplyHere(*Spec);
		if (!Handle.IsValid()) continue;
		Report.Restored++;
		const float Elapsed = A.Elapsed + Transit;
		if (A.Duration > 0.f && Elapsed > 0.f) ModifyActiveEffectStartTime(Handle, -Elapsed);
		FActiveGameplayEffect* Active = ActiveGameplayEffects.GetActiveGameplayEffect(Handle);
		if (!Active) continue;
		if (A.Duration > 0.f)
		{
			// The end it should have, on the shared clock, against the end it got.
			const float Left = Active->GetTimeRemaining(float(World->GetTimeSeconds()));
			const double ShouldEnd = State.SentAt + (A.Duration - A.Elapsed);
			Report.EndErrors.Add(float((NowShared + Left) - ShouldEnd));
		}
		if (A.PeriodRemaining >= 0.f && Active->GetPeriod() > 0.f && Active->PeriodHandle.IsValid())
		{
			// GAS has no API for the period phase: the next tick comes where it would have on the old server.
			float First = A.PeriodRemaining - Transit;
			while (First <= 0.f) First += Active->GetPeriod();
			Timers.SetTimer(Active->PeriodHandle, FTimerDelegate::CreateUObject(this, &UGasLabAbilitySystemComponent::ExecutePeriodicEffect, Handle), Active->GetPeriod(), true, First);
			const double ShouldTick = State.SentAt + A.PeriodRemaining;
			double Tick = ShouldTick;
			while (Tick < NowShared) Tick += Active->GetPeriod();
			Report.PhaseErrors.Add(float((NowShared + Timers.GetTimerRemaining(Active->PeriodHandle)) - Tick));
		}
	}
}

void UGasLabAbilitySystemComponent::Mirror(const FGasLabWireGhost& Ghost)
{
	SetNumericAttributeBase(UGasLabAttributeSet::GetMaxHealthAttribute(), Ghost.MaxHealth);
	SetNumericAttributeBase(UGasLabAttributeSet::GetHealthAttribute(), Ghost.Health);

	TSet<FName> Tags(Ghost.Tags), Cues(Ghost.Cues);
	for (const FName& Name : Tags)
	{
		if (MirroredTags.Contains(Name)) continue;
		const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(Name, false);
		if (Tag.IsValid()) AddLooseGameplayTag(Tag, 1, EGameplayTagReplicationState::TagOnly);
	}
	for (const FName& Name : MirroredTags)
	{
		if (Tags.Contains(Name)) continue;
		const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(Name, false);
		if (Tag.IsValid()) RemoveLooseGameplayTag(Tag, 1, EGameplayTagReplicationState::TagOnly);
	}
	MirroredTags = MoveTemp(Tags);

	// Looping cues are added here, by this server, so they replicate to its own clients like any cue it owns.
	for (const FName& Name : Cues)
	{
		if (MirroredCues.Contains(Name)) continue;
		const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(Name, false);
		if (Tag.IsValid()) AddGameplayCue(Tag);
	}
	for (const FName& Name : MirroredCues)
	{
		if (Cues.Contains(Name)) continue;
		const FGameplayTag Tag = FGameplayTag::RequestGameplayTag(Name, false);
		if (Tag.IsValid()) RemoveGameplayCue(Tag);
	}
	MirroredCues = MoveTemp(Cues);
}

TArray<FName> UGasLabAbilitySystemComponent::ActiveCueNames() const
{
	TSet<FName> Names;
	for (const FActiveGameplayEffect& Active : &ActiveGameplayEffects)
	{
		if (Active.IsPendingRemove || !Active.Spec.Def) continue;
		for (const FGameplayEffectCue& Cue : Active.Spec.Def->GameplayCues)
		{
			for (const FGameplayTag& Tag : Cue.GameplayCueTags) Names.Add(Tag.GetTagName());
		}
	}
	for (const FActiveGameplayCue& Cue : ActiveGameplayCues.GameplayCues) Names.Add(Cue.GameplayCueTag.GetTagName());
	return Names.Array();
}

const FActiveGameplayEffect* UGasLabAbilitySystemComponent::FindActive(const FGameplayTag& Tag) const
{
	for (const FActiveGameplayEffect& Active : &ActiveGameplayEffects)
	{
		if (Active.IsPendingRemove || !Active.Spec.Def) continue;
		if (Active.Spec.Def->GetAssetTags().HasTag(Tag) || Active.Spec.Def->GetGrantedTags().HasTag(Tag)) return &Active;
	}
	return nullptr;
}

float UGasLabAbilitySystemComponent::TimeLeft(const FGameplayTag& Tag) const
{
	const FActiveGameplayEffect* Active = FindActive(Tag);
	const UWorld* World = GetWorld();
	return (Active && World) ? Active->GetTimeRemaining(float(World->GetTimeSeconds())) : -1.f;
}

int32 UGasLabAbilitySystemComponent::Stacks(const FGameplayTag& Tag) const
{
	const FActiveGameplayEffect* Active = FindActive(Tag);
	return Active ? Active->Spec.GetStackCount() : 0;
}

float UGasLabAbilitySystemComponent::NextTickIn(const FGameplayTag& Tag) const
{
	const FActiveGameplayEffect* Active = FindActive(Tag);
	const UWorld* World = GetWorld();
	return (Active && World && Active->GetPeriod() > 0.f) ? World->GetTimerManager().GetTimerRemaining(Active->PeriodHandle) : -1.f;
}
