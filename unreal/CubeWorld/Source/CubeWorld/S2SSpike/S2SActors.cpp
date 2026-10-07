#include "S2SActors.h"
#include "AbilitySystemComponent.h"
#include "Components/SceneComponent.h"
#include "Engine/World.h"
#include "GameplayEffectExtension.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Net/UnrealNetwork.h"

namespace
{
	FString DescribeNet(const AActor* Actor)
	{
		const UWorld* World = Actor->GetWorld();
		const TCHAR* Mode = TEXT("?");
		switch (Actor->GetNetMode())
		{
		case NM_Standalone: Mode = TEXT("Standalone"); break;
		case NM_DedicatedServer: Mode = TEXT("DedicatedServer"); break;
		case NM_ListenServer: Mode = TEXT("ListenServer"); break;
		case NM_Client: Mode = TEXT("Client"); break;
		default: break;
		}
		return FString::Printf(TEXT("role=%s remote=%s netdriver=%s netmode=%s world=%s"),
			*S2SStats::RoleName(Actor->GetLocalRole()), *S2SStats::RoleName(Actor->GetRemoteRole()),
			*Actor->GetNetDriverName().ToString(), Mode, World ? *World->GetName() : TEXT("-"));
	}
}

// ---------------------------------------------------------------------------------------------------------------------

AS2SProjectile::AS2SProjectile()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;
	SetReplicatingMovement(true);
	bAlwaysRelevant = true;
	SetNetUpdateFrequency(30.f);
	SetMinNetUpdateFrequency(30.f);

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Root->SetMobility(EComponentMobility::Movable);
	RootComponent = Root;
}

void AS2SProjectile::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AS2SProjectile, Origin);
	DOREPLIFETIME(AS2SProjectile, Velocity);
	DOREPLIFETIME(AS2SProjectile, HalfLane);
	DOREPLIFETIME(AS2SProjectile, Payload);
	DOREPLIFETIME(AS2SProjectile, Stamp);
}

void AS2SProjectile::BeginPlay()
{
	Super::BeginPlay();
	if (HasAuthority())
	{
		UE_LOG(LogS2S, Log, TEXT("[S2S %s] projectile %s spawned at %s, mirrored=%d (%s)"), *S2SStats::Who(), *GetName(),
			*GetActorLocation().ToCompactString(), bMirrorToPeers ? 1 : 0, *DescribeNet(this));
	}
	else
	{
		UE_LOG(LogS2S, Display, TEXT("[S2S %s] COPY ARRIVED projectile %s origin=%s at %s, payload %s level %.0f sbc=%d instigator=%s (%s)"),
			*S2SStats::Who(), *GetName(), *Origin, *GetActorLocation().ToCompactString(), *Payload.EffectClass.ToString(), Payload.Level,
			Payload.SetByCallers.Num(), *Payload.InstigatorId, *DescribeNet(this));
	}
}

void AS2SProjectile::EndPlay(const EEndPlayReason::Type Reason)
{
	if (!HasAuthority())
	{
		UE_LOG(LogS2S, Display, TEXT("[S2S %s] COPY GONE projectile %s (reason %d), updates received %d, seq gaps %d"),
			*S2SStats::Who(), *GetName(), (int32)Reason, ReceivedUpdates, SkippedSeqs);
	}
	Super::EndPlay(Reason);
}

void AS2SProjectile::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!HasAuthority() || bS2SCopy) return;

	FVector Pos = GetActorLocation() + Velocity * DeltaSeconds;
	if (Pos.X > HalfLane) Pos.X -= 2.0 * HalfLane;
	if (Pos.X < -HalfLane) Pos.X += 2.0 * HalfLane;
	SetActorLocation(Pos);

	Stamp.SentAt = FPlatformTime::Seconds();
	Stamp.Seq++;
	Stamp.AuthorityPos = Pos;
}

void AS2SProjectile::OnRep_Stamp()
{
	const double Delay = FPlatformTime::Seconds() - Stamp.SentAt;
	ReceivedUpdates++;
	if (LastSeq > 0 && Stamp.Seq > LastSeq + 1) SkippedSeqs += Stamp.Seq - LastSeq - 1;
	LastSeq = Stamp.Seq;
	// The first update is the initial state, sent when the link came up: not a measure of the update path.
	if (ReceivedUpdates > 1) S2SStats::RecordLatency(*FString::Printf(TEXT("projectile from %s"), *Origin), Delay);

	if (ReceivedUpdates <= 3 || ReceivedUpdates % 300 == 0)
	{
		UE_LOG(LogS2S, Log, TEXT("[S2S %s] projectile %s update #%d seq %d: delay %.1f ms, here %s, authority stamped %s (off by %.1f cm)"),
			*S2SStats::Who(), *GetName(), ReceivedUpdates, Stamp.Seq, 1000.0 * Delay, *GetActorLocation().ToCompactString(),
			*Stamp.AuthorityPos.ToCompactString(), FVector::Dist(GetActorLocation(), Stamp.AuthorityPos));
	}
}

// ---------------------------------------------------------------------------------------------------------------------

void US2SAttributeSet::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME_CONDITION_NOTIFY(US2SAttributeSet, Health, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME(US2SAttributeSet, ChangeStamp);
}

void US2SAttributeSet::PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data)
{
	Super::PostGameplayEffectExecute(Data);
	if (Data.EvaluatedData.Attribute == GetHealthAttribute())
	{
		ChangeStamp.SentAt = FPlatformTime::Seconds();
		ChangeStamp.Seq++;
	}
}

void US2SAttributeSet::OnRep_Health(const FGameplayAttributeData& OldValue)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(US2SAttributeSet, Health, OldValue);

	if (ChangeStamp.Seq != LastSeenSeq && ChangeStamp.SentAt > 0.0)
	{
		LastSeenSeq = ChangeStamp.Seq;
		ReceivedChanges++;
		const double Delay = FPlatformTime::Seconds() - ChangeStamp.SentAt;
		// The first one carries the state from before the copy existed (its stamp is of the last change before that).
		const AS2SAscActor* Owner = Cast<AS2SAscActor>(GetOwningActor());
		if (ReceivedChanges > 1) S2SStats::RecordLatency(*FString::Printf(TEXT("gas-health from %s"), Owner ? *Owner->Origin : TEXT("?")), Delay);
		if (ReceivedChanges <= 3 || ReceivedChanges % 20 == 0)
		{
			UE_LOG(LogS2S, Log, TEXT("[S2S %s] attribute copy: Health %.1f -> %.1f (base %.1f), change #%d seq %d, delay %.1f ms"),
				*S2SStats::Who(), OldValue.GetCurrentValue(), Health.GetCurrentValue(), Health.GetBaseValue(), ReceivedChanges,
				ChangeStamp.Seq, 1000.0 * Delay);
		}
	}
}

// ---------------------------------------------------------------------------------------------------------------------

US2SDamageEffect::US2SDamageEffect()
{
	DurationPolicy = EGameplayEffectDurationType::Instant;

	FSetByCallerFloat Damage;
	Damage.DataTag = TAG_S2S_SetByCaller_Damage;

	FGameplayModifierInfo Modifier;
	Modifier.Attribute = US2SAttributeSet::GetHealthAttribute();
	Modifier.ModifierOp = EGameplayModOp::AddBase;
	Modifier.ModifierMagnitude = FGameplayEffectModifierMagnitude(Damage);
	Modifiers.Add(Modifier);
}

US2SBuffEffect::US2SBuffEffect()
{
	DurationPolicy = EGameplayEffectDurationType::Infinite;

	FGameplayModifierInfo Modifier;
	Modifier.Attribute = US2SAttributeSet::GetHealthAttribute();
	Modifier.ModifierOp = EGameplayModOp::AddBase;
	Modifier.ModifierMagnitude = FGameplayEffectModifierMagnitude(FScalableFloat(100.f));
	Modifiers.Add(Modifier);
}

// ---------------------------------------------------------------------------------------------------------------------

AS2SAscActor::AS2SAscActor()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;
	bAlwaysRelevant = true;
	SetNetUpdateFrequency(30.f);
	SetMinNetUpdateFrequency(30.f);

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Root->SetMobility(EComponentMobility::Movable);
	RootComponent = Root;

	AbilitySystem = CreateDefaultSubobject<UAbilitySystemComponent>(TEXT("AbilitySystem"));
	AbilitySystem->SetIsReplicated(true);
	// The ASC finds this set among its owner's subobjects when it initialises (UAbilitySystemComponent::InitializeComponent).
	Attributes = CreateDefaultSubobject<US2SAttributeSet>(TEXT("Attributes"));
}

void AS2SAscActor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AS2SAscActor, Origin);
}

void AS2SAscActor::BeginPlay()
{
	Super::BeginPlay();
	AbilitySystem->InitAbilityActorInfo(this, this);

	if (HasAuthority())
	{
		FString ModeText = TEXT("Mixed");
		FParse::Value(FCommandLine::Get(), TEXT("-s2sascmode="), ModeText);
		const EGameplayEffectReplicationMode Mode = ModeText.Equals(TEXT("Full"), ESearchCase::IgnoreCase) ? EGameplayEffectReplicationMode::Full
			: ModeText.Equals(TEXT("Minimal"), ESearchCase::IgnoreCase) ? EGameplayEffectReplicationMode::Minimal
			: EGameplayEffectReplicationMode::Mixed;
		AbilitySystem->SetReplicationMode(Mode);

		AbilitySystem->SetNumericAttributeBase(US2SAttributeSet::GetHealthAttribute(), 1000.f);
		AbilitySystem->ApplyGameplayEffectToSelf(GetDefault<US2SBuffEffect>(), 1.f, AbilitySystem->MakeEffectContext());
		AbilitySystem->AddLooseGameplayTag(TAG_S2S_State_Burning, 1, EGameplayTagReplicationState::TagOnly);
		AbilitySystem->AddGameplayCue(TAG_S2S_Cue_Loop);

		const double Now = GetWorld()->GetTimeSeconds();
		NextDamageAt = Now + 1.0;
		NextBurstAt = Now + 2.0;
		UE_LOG(LogS2S, Log, TEXT("[S2S %s] ASC actor %s up, replication mode %s, mirrored=%d: %s"), *S2SStats::Who(), *GetName(),
			*ModeText, bMirrorToPeers ? 1 : 0, *Describe());
	}
	else
	{
		UE_LOG(LogS2S, Display, TEXT("[S2S %s] COPY ARRIVED ASC actor %s (%s): %s"), *S2SStats::Who(), *GetName(), *DescribeNet(this), *Describe());
	}
}

void AS2SAscActor::EndPlay(const EEndPlayReason::Type Reason)
{
	if (!HasAuthority())
	{
		UE_LOG(LogS2S, Display, TEXT("[S2S %s] COPY GONE ASC actor %s (reason %d): %s"), *S2SStats::Who(), *GetName(), (int32)Reason, *Describe());
	}
	Super::EndPlay(Reason);
}

void AS2SAscActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!HasAuthority() || bS2SCopy || !bRunAuthorityTimers) return;

	const double Now = GetWorld()->GetTimeSeconds();
	if (Now >= NextDamageAt)
	{
		NextDamageAt = Now + 1.0;
		FGameplayEffectSpecHandle Spec = AbilitySystem->MakeOutgoingSpec(US2SDamageEffect::StaticClass(), 1.f, AbilitySystem->MakeEffectContext());
		Spec.Data->SetSetByCallerMagnitude(TAG_S2S_SetByCaller_Damage, -5.f);
		AbilitySystem->ApplyGameplayEffectSpecToSelf(*Spec.Data);
	}
	if (Now >= NextBurstAt)
	{
		NextBurstAt = Now + 2.0;
		AbilitySystem->ExecuteGameplayCue(TAG_S2S_Cue_Burst);
	}
}

FString AS2SAscActor::Describe() const
{
	const UAbilitySystemComponent* Asc = AbilitySystem;
	if (!Asc) return TEXT("no ASC");
	return FString::Printf(TEXT("origin=%s role=%s Health=%.1f (base %.1f) tags=[%s] activeEffects=%d ownerAuthoritative=%d"),
		*Origin, *S2SStats::RoleName(GetLocalRole()),
		Asc->GetNumericAttribute(US2SAttributeSet::GetHealthAttribute()),
		Asc->GetNumericAttributeBase(US2SAttributeSet::GetHealthAttribute()),
		*Asc->GetOwnedGameplayTags().ToStringSimple(),
		Asc->GetActiveGameplayEffects().GetNumGameplayEffects(),
		Asc->IsOwnerActorAuthoritative() ? 1 : 0);
}
