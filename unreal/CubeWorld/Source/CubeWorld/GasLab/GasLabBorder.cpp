#include "GasLab/GasLabBorder.h"
#include "GasLab/GasLabAbilitySystemComponent.h"
#include "GasLab/GasLabAttributeSet.h"
#include "GasLab/GasLabCharacter.h"
#include "GasLab/GasLabClock.h"
#include "GasLab/GasLabCreeper.h"
#include "GasLab/GasLabEffectContext.h"
#include "GasLab/GasLabGameMode.h"
#include "GasLab/GasLabPlayerController.h"
#include "GasLab/GasLabPlayerState.h"
#include "CubeShapes.h"
#include "EngineUtils.h"
#include "GameplayEffect.h"
#include "AbilitySystemGlobals.h"
#include "GameplayAbilitiesDeveloperSettings.h"
#include "Misc/ConfigCacheIni.h"
#include "NativeGameplayTags.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GasLabBorder)

UE_DEFINE_GAMEPLAY_TAG_STATIC(TAG_GasLab_Handoff, "State.Handoff");

namespace
{
	FString JoinFloats(const TArray<float>& Values)
	{
		TArray<FString> Parts;
		for (float V : Values) Parts.Add(FString::Printf(TEXT("%.1f"), V * 1000.f));
		return FString::Join(Parts, TEXT(","));
	}

	FString EffectName(const FGameplayEffectSpec& Spec)
	{
		return Spec.Def ? Spec.Def->GetClass()->GetName().Replace(TEXT("GasLabGE_"), TEXT("")) : FString(TEXT("?"));
	}
}

UGasLabBorder* UGasLabBorder::Get(const UObject* WorldContext)
{
	const UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
	return World ? World->GetSubsystem<UGasLabBorder>() : nullptr;
}

bool UGasLabBorder::IsLab()
{
	static const bool bLab = FParse::Param(FCommandLine::Get(), TEXT("gaslab"));
	return bLab;
}

FGameplayTag UGasLabBorder::HandoffTag()
{
	return TAG_GasLab_Handoff;
}

int32 UGasLabBorder::RegionAt(const FVector& P)
{
	const int32 Col = FMath::Clamp(FMath::FloorToInt(P.X / RegionSize), 0, Columns - 1);
	const int32 Row = FMath::Clamp(FMath::FloorToInt(P.Y / RegionSize), 0, Rows - 1);
	return Row * Columns + Col;
}

FBox2D UGasLabBorder::RegionBox(int32 R)
{
	const int32 Col = R % Columns, Row = R / Columns;
	return FBox2D(FVector2D(Col * RegionSize, Row * RegionSize), FVector2D((Col + 1) * RegionSize, (Row + 1) * RegionSize));
}

FVector UGasLabBorder::RegionCenter(int32 R)
{
	const FVector2D C = RegionBox(R).GetCenter();
	return FVector(C.X, C.Y, 100.f);
}

float UGasLabBorder::DistanceToRegion(const FVector& P, int32 R)
{
	return FMath::Sqrt(RegionBox(R).ComputeSquaredDistanceToPoint(FVector2D(P.X, P.Y)));
}

FLinearColor UGasLabBorder::RegionColor(int32 R)
{
	switch (R)
	{
	case 0: return FCubeShape::Hex(TEXT("#ef4444"));
	case 1: return FCubeShape::Hex(TEXT("#3b82f6"));
	case 2: return FCubeShape::Hex(TEXT("#22c55e"));
	case 3: return FCubeShape::Hex(TEXT("#eab308"));
	default: return FCubeShape::Hex(TEXT("#9ca3af"));
	}
}

bool UGasLabBorder::ShouldCreateSubsystem(UObject* Outer) const
{
	return IsLab() && Super::ShouldCreateSubsystem(Outer);
}

void UGasLabBorder::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	if (InWorld.GetNetMode() != NM_DedicatedServer || !InWorld.IsGameWorld()) return;
	FParse::Value(FCommandLine::Get(), TEXT("-region="), Region);
	FString ModeText;
	if (FParse::Value(FCommandLine::Get(), TEXT("-gasmode="), ModeText) && ModeText == TEXT("area")) Mode = EGasLabMode::Area;
	FParse::Value(FCommandLine::Get(), TEXT("-gasband="), GhostBand);
	FString List;
	if (FParse::Value(FCommandLine::Get(), TEXT("-peers="), List, false))
	{
		TArray<FString> Items;
		List.ParseIntoArray(Items, TEXT(","));
		for (const FString& Item : Items)
		{
			FString RegionText, Address, Host, PortText;
			if (!Item.Split(TEXT("@"), &RegionText, &Address) || !Address.Split(TEXT(":"), &Host, &PortText)) continue;
			FGasLabPeer Peer;
			Peer.Region = FCString::Atoi(*RegionText);
			Peer.Host = Host;
			// The server-to-server link listens 100 above the game port.
			Peer.Port = FCString::Atoi(*PortText) + 100;
			Peers.Add(Peer);
		}
	}
	Link = IGasLabLink::Make();
	Link->OnReceive = [this](int32 From, const TArray<uint8>& Bytes) { Hear(From, Bytes); };
	Link->Start(Region, Peers);
	StatAt = FPlatformTime::Seconds();
	{
		FString Configured;
		GConfig->GetString(TEXT("/Script/GameplayAbilities.GameplayAbilitiesDeveloperSettings"), TEXT("AbilitySystemGlobalsClassName"), Configured, GGameIni);
		UE_LOG(LogTemp, Display, TEXT("GASLAB globals-config ini=%s settings=%s gameini=%s"), *Configured,
			*GetDefault<UGameplayAbilitiesDeveloperSettings>()->AbilitySystemGlobalsClassName.ToString(), *GGameIni);
	}
	UE_LOG(LogTemp, Display, TEXT("GASLAB start region=%d globals=%s mode=%s link=%s peers=%d band=%.0f clock=%s epoch=%lld delay=%.1fms"),
		Region, *UAbilitySystemGlobals::Get().GetClass()->GetName(), Mode == EGasLabMode::Area ? TEXT("area") : TEXT("forward"), *Link->Name(), Peers.Num(), GhostBand,
		GasLabClock::IsShared() ? TEXT("shared") : TEXT("local"), GasLabClock::Epoch(), IGasLabLink::Delay() * 1000.0);
}

void UGasLabBorder::Deinitialize()
{
	Link.Reset();
	Super::Deinitialize();
}

TStatId UGasLabBorder::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UGasLabBorder, STATGROUP_Tickables);
}

bool UGasLabBorder::AllConnected() const
{
	if (!Link.IsValid()) return false;
	for (const FGasLabPeer& P : Peers)
	{
		if (P.Region != Region && !Link->IsConnected(P.Region)) return false;
	}
	return true;
}

void UGasLabBorder::AnnounceStart(double At)
{
	ScenarioStart = At;
	for (const FGasLabPeer& P : Peers)
	{
		if (P.Region == Region) continue;
		double Stamp = At;
		Send(P.Region, EGasLabMsg::Start, [&Stamp](FArchive& Ar) { Ar << Stamp; });
	}
}

FString UGasLabBorder::AddressOf(int32 OtherRegion) const
{
	for (const FGasLabPeer& P : Peers)
	{
		if (P.Region == OtherRegion) return FString::Printf(TEXT("%s:%d"), *P.Host, P.Port - 100);
	}
	return FString();
}

void UGasLabBorder::Send(int32 To, EGasLabMsg Type, TFunctionRef<void(FArchive&)> Body)
{
	if (!Link.IsValid() || To == Region) return;
	TArray<uint8> Bytes;
	FMemoryWriter Writer(Bytes);
	uint8 T = uint8(Type);
	Writer << T;
	Body(Writer);
	Link->Send(To, Bytes);
}

void UGasLabBorder::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (!Link.IsValid()) return;
	Link->Tick();
	const double Now = FPlatformTime::Seconds();
	if (Now >= NextGhosts)
	{
		NextGhosts = Now + 0.05;
		SendGhosts();
		ExpireGhosts();
		CheckCrossings();
	}
	if (Now >= NextProbe)
	{
		NextProbe = Now + 1.0;
		const double At = GasLabClock::Now();
		for (const FGasLabPeer& P : Peers)
		{
			if (P.Region == Region) continue;
			double Stamp = At;
			Send(P.Region, EGasLabMsg::Probe, [&Stamp](FArchive& Ar) { Ar << Stamp; });
		}
	}
	// Bots whose handoff was never acknowledged stay here.
	for (auto It = BotsLeaving.CreateIterator(); It; ++It)
	{
		if (Now - It->Value < 3.0) continue;
		if (UGasLabAbilitySystemComponent* ASC = FindLocal(It->Key))
		{
			ASC->bLeaving = false;
			ASC->RemoveLooseGameplayTag(HandoffTag());
		}
		Departed.Remove(It->Key);
		UE_LOG(LogTemp, Warning, TEXT("GASLAB handoff-timeout id=%s at=%.3f"), *It->Key, GasLabClock::Now());
		It.RemoveCurrent();
	}
	for (auto It = ArrivalHeard.CreateIterator(); It; ++It)
	{
		if (Now - It->Value < 20.0) continue;
		Arrivals.Remove(It->Key);
		EffectsWaiting.Remove(It->Key);
		It.RemoveCurrent();
	}
	if (Now >= NextStats)
	{
		NextStats = Now + 5.0;
		LogStats();
	}
}

void UGasLabBorder::LogStats()
{
	const double Now = FPlatformTime::Seconds();
	const double Span = FMath::Max(0.001, Now - StatAt);
	int32 Local = 0, Ghosts = 0;
	for (TActorIterator<AGasLabCharacter> It(GetWorld()); It; ++It) (It->IsGhost() ? Ghosts : Local)++;
	UE_LOG(LogTemp, Display, TEXT("GASLAB stats region=%d link=%s out=%.0fB/s in=%.0fB/s framesout=%.1f/s framesin=%.1f/s players=%d ghosts=%d at=%.3f"),
		Region, *Link->Name(), (Link->BytesOut - StatBytesOut) / Span, (Link->BytesIn - StatBytesIn) / Span,
		(Link->FramesOut - StatFramesOut) / Span, (Link->FramesIn - StatFramesIn) / Span, Local, Ghosts, GasLabClock::Now());
	StatBytesOut = Link->BytesOut; StatBytesIn = Link->BytesIn; StatFramesOut = Link->FramesOut; StatFramesIn = Link->FramesIn;
	StatAt = Now;
}

UGasLabAbilitySystemComponent* UGasLabBorder::FindLocal(const FString& Id) const
{
	AGasLabCharacter* C = FindCharacter(Id, false);
	return C ? C->GetLabASC() : nullptr;
}

AGasLabCharacter* UGasLabBorder::FindCharacter(const FString& Id, bool bGhost) const
{
	for (TActorIterator<AGasLabCharacter> It(GetWorld()); It; ++It)
	{
		if (It->IsGhost() == bGhost && It->GetPlayerId() == Id && !It->IsActorBeingDestroyed()) return *It;
	}
	return nullptr;
}

// ── ghosts ───────────────────────────────────────────────────────────────────────────────────────

void UGasLabBorder::SendGhosts()
{
	TMap<int32, TArray<FGasLabWireGhost>> Batches;
	const double Now = GasLabClock::Now();
	for (TActorIterator<AGasLabCharacter> It(GetWorld()); It; ++It)
	{
		AGasLabCharacter* C = *It;
		UGasLabAbilitySystemComponent* ASC = C->GetLabASC();
		if (C->IsGhost() || !ASC || ASC->bLeaving) continue;
		const FVector P = C->GetActorLocation();
		FGasLabWireGhost G;
		bool bMade = false;
		for (const FGasLabPeer& Peer : Peers)
		{
			if (Peer.Region == Region || DistanceToRegion(P, Peer.Region) > GhostBand) continue;
			if (!bMade)
			{
				bMade = true;
				G.Id = C->GetPlayerId();
				G.Name = C->GetPlayerState() ? C->GetPlayerState()->GetPlayerName() : G.Id;
				G.OwnerRegion = Region;
				G.Location = P;
				G.Velocity = C->GetVelocity();
				G.Yaw = C->GetActorRotation().Yaw;
				G.Health = ASC->GetNumericAttribute(UGasLabAttributeSet::GetHealthAttribute());
				G.MaxHealth = ASC->GetNumericAttribute(UGasLabAttributeSet::GetMaxHealthAttribute());
				FGameplayTagContainer Owned;
				ASC->GetOwnedGameplayTags(Owned);
				G.Tags = GasLabWire::TagNames(Owned);
				G.Cues = ASC->ActiveCueNames();
				G.SentAt = Now;
			}
			Batches.FindOrAdd(Peer.Region).Add(G);
		}
	}
	for (TPair<int32, TArray<FGasLabWireGhost>>& B : Batches)
	{
		Send(B.Key, EGasLabMsg::Ghosts, [&B](FArchive& Ar) { Ar << B.Value; });
	}
}

void UGasLabBorder::HearGhosts(int32 From, TArray<FGasLabWireGhost>& Ghosts)
{
	UWorld* World = GetWorld();
	for (FGasLabWireGhost& G : Ghosts)
	{
		// Ours now, or on its way here: the real one wins over any ghost.
		if (FindCharacter(G.Id, false) || Arrivals.Contains(G.Id)) { DropGhost(G.Id); continue; }
		AGasLabCharacter* Ghost = FindCharacter(G.Id, true);
		if (!Ghost)
		{
			FActorSpawnParameters Params;
			Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			AGasLabPlayerState* PS = World->SpawnActor<AGasLabPlayerState>(AGasLabPlayerState::StaticClass(), FTransform::Identity, Params);
			Ghost = World->SpawnActor<AGasLabCharacter>(AGasLabCharacter::StaticClass(), G.Location, FRotator(0, G.Yaw, 0), Params);
			if (!PS || !Ghost) { if (PS) PS->Destroy(); if (Ghost) Ghost->Destroy(); continue; }
			PS->LabId = G.Id;
			PS->bGhost = true;
			PS->OwnerRegion = G.OwnerRegion;
			PS->SetPlayerName(G.Name);
			PS->GetLabASC()->PlayerId = G.Id;
			PS->GetLabASC()->bGhost = true;
			Ghost->SetPlayerState(PS);
			Ghost->InitAbilityActorInfo();
			UE_LOG(LogTemp, Log, TEXT("GASLAB ghost-new id=%s owner=%d at=%.3f"), *G.Id, G.OwnerRegion, GasLabClock::Now());
		}
		AGasLabPlayerState* PS = Ghost->GetPlayerState<AGasLabPlayerState>();
		if (PS && PS->OwnerRegion != G.OwnerRegion) PS->OwnerRegion = G.OwnerRegion;
		if (UGasLabAbilitySystemComponent* ASC = Ghost->GetLabASC())
		{
			ASC->OwnerRegion = G.OwnerRegion;
			ASC->Mirror(G);
		}
		Ghost->SetGhostTarget(G.Location, G.Velocity, G.Yaw);
		GhostHeard.Add(G.Id, FPlatformTime::Seconds());
	}
}

void UGasLabBorder::ExpireGhosts()
{
	const double Now = FPlatformTime::Seconds();
	TArray<FString> Stale;
	for (const TPair<FString, double>& G : GhostHeard) if (Now - G.Value > 1.0) Stale.Add(G.Key);
	for (const FString& Id : Stale) DropGhost(Id);
}

void UGasLabBorder::DropGhost(const FString& Id)
{
	GhostHeard.Remove(Id);
	if (AGasLabCharacter* Ghost = FindCharacter(Id, true))
	{
		if (APlayerState* PS = Ghost->GetPlayerState()) PS->Destroy();
		Ghost->Destroy();
		UE_LOG(LogTemp, Log, TEXT("GASLAB ghost-gone id=%s at=%.3f"), *Id, GasLabClock::Now());
	}
}

// ── effects and credit ───────────────────────────────────────────────────────────────────────────

void UGasLabBorder::ForwardEffect(UGasLabAbilitySystemComponent& Target, const FGameplayEffectSpec& Spec)
{
	const int32* Went = Departed.Find(Target.PlayerId);
	const int32 To = Target.bGhost ? Target.OwnerRegion : (Went ? *Went : -1);
	if (To < 0 || To == Region)
	{
		UE_LOG(LogTemp, Warning, TEXT("GASLAB effect-lost target=%s effect=%s why=no-owner at=%.3f"), *Target.PlayerId, *EffectName(Spec), GasLabClock::Now());
		return;
	}
	FString Id = Target.PlayerId;
	FGasLabWireSpec Wire = FGasLabWireSpec::From(Spec);
	double SentAt = GasLabClock::Now();
	Send(To, EGasLabMsg::Effect, [&](FArchive& Ar) { Ar << Id << Wire << SentAt; });
	UE_LOG(LogTemp, Log, TEXT("GASLAB forward target=%s effect=%s to=%d leaving=%d at=%.3f"), *Id, *EffectName(Spec), To, Target.bLeaving ? 1 : 0, SentAt);
}

void UGasLabBorder::HearEffect(int32 From, const FString& TargetId, FGasLabWireSpec& Wire, double SentAt)
{
	const double Now = GasLabClock::Now();
	UGasLabAbilitySystemComponent* ASC = FindLocal(TargetId);
	if (ASC && !ASC->bLeaving)
	{
		TSharedPtr<FGameplayEffectSpec> Spec = Wire.Build(nullptr);
		if (!Spec) return;
		UE_LOG(LogTemp, Log, TEXT("GASLAB forwarded-in target=%s effect=%s from=%d link=%.1fms at=%.3f"), *TargetId, *EffectName(*Spec), From, (Now - SentAt) * 1000.0, Now);
		ASC->ApplyHere(*Spec);
		return;
	}
	if (const int32* Went = Departed.Find(TargetId))
	{
		int32 To = *Went;
		FString Id = TargetId;
		Send(To, EGasLabMsg::Effect, [&](FArchive& Ar) { Ar << Id << Wire << SentAt; });
		UE_LOG(LogTemp, Log, TEXT("GASLAB reforward target=%s to=%d at=%.3f"), *TargetId, To, Now);
		return;
	}
	if (Arrivals.Contains(TargetId))
	{
		EffectsWaiting.FindOrAdd(TargetId).Add(Wire);
		UE_LOG(LogTemp, Log, TEXT("GASLAB effect-waits target=%s at=%.3f"), *TargetId, Now);
		return;
	}
	UE_LOG(LogTemp, Warning, TEXT("GASLAB effect-lost target=%s from=%d why=unknown at=%.3f"), *TargetId, From, Now);
}

void UGasLabBorder::NoteDamage(UGasLabAbilitySystemComponent& Target, const FGameplayEffectSpec& Spec, float Lost, bool bKilled)
{
	const FGasLabEffectContext* Context = FGasLabEffectContext::From(Spec.GetEffectContext());
	const double Now = GasLabClock::Now();
	if (!Context)
	{
		const FGameplayEffectContext* Raw = Spec.GetEffectContext().Get();
		UE_LOG(LogTemp, Warning, TEXT("GASLAB context-type %s"), Raw ? *Raw->GetScriptStruct()->GetName() : TEXT("none"));
	}
	const bool bPeriodic = Spec.GetPeriod() > 0.f;
	const float Lag = (Context && Context->EventAt > 0 && !bPeriodic) ? float((Now - Context->EventAt) * 1000.0) : -1.f;
	UE_LOG(LogTemp, Log, TEXT("GASLAB hit target=%s by=%s effect=%s dmg=%.2f hp=%.2f killed=%d region=%d lag=%.1fms at=%.3f"),
		*Target.PlayerId, Context ? *Context->InstigatorId : TEXT(""), *EffectName(Spec), Lost,
		Target.GetNumericAttribute(UGasLabAttributeSet::GetHealthAttribute()), bKilled ? 1 : 0, Region, Lag, Now);
	if (!Context || Context->InstigatorId.IsEmpty()) return;
	if (UGasLabAbilitySystemComponent* Instigator = FindLocal(Context->InstigatorId))
	{
		Instigator->Credit(Target.PlayerId, Lost, bKilled, false);
		return;
	}
	const int32* Went = Departed.Find(Context->InstigatorId);
	const int32 To = Went ? *Went : Context->OriginRegion;
	if (To < 0 || To == Region) return;
	FGasLabWireCredit Credit;
	Credit.InstigatorId = Context->InstigatorId;
	Credit.TargetId = Target.PlayerId;
	Credit.Damage = Lost;
	Credit.bKilled = bKilled;
	Credit.At = Now;
	Send(To, EGasLabMsg::Credit, [&Credit](FArchive& Ar) { Ar << Credit; });
}

void UGasLabBorder::HearCredit(int32 From, FGasLabWireCredit& Credit)
{
	if (UGasLabAbilitySystemComponent* Instigator = FindLocal(Credit.InstigatorId))
	{
		Instigator->Credit(Credit.TargetId, Credit.Damage, Credit.bKilled, true);
		UE_LOG(LogTemp, Log, TEXT("GASLAB credit by=%s target=%s dmg=%.2f killed=%d from=%d lag=%.1fms total=%.2f at=%.3f"),
			*Credit.InstigatorId, *Credit.TargetId, Credit.Damage, Credit.bKilled ? 1 : 0, From, (GasLabClock::Now() - Credit.At) * 1000.0, Instigator->DamageDealt, GasLabClock::Now());
		return;
	}
	if (const int32* Went = Departed.Find(Credit.InstigatorId))
	{
		int32 To = *Went;
		Send(To, EGasLabMsg::Credit, [&Credit](FArchive& Ar) { Ar << Credit; });
		return;
	}
	UE_LOG(LogTemp, Warning, TEXT("GASLAB credit-lost by=%s from=%d at=%.3f"), *Credit.InstigatorId, From, GasLabClock::Now());
}

// ── creepers ─────────────────────────────────────────────────────────────────────────────────────

void UGasLabBorder::ShareCreeper(const AGasLabCreeper& Creeper)
{
	if (Creeper.bGhost) return;
	FGasLabWireCreeper C;
	C.Id = Creeper.Id;
	C.ThrowerId = Creeper.ThrowerId;
	C.OwnerRegion = Region;
	C.Origin = Creeper.Origin;
	C.Velocity = Creeper.Velocity;
	C.LaunchAt = Creeper.LaunchAt;
	C.bExploded = Creeper.bExploded;
	C.ExplodedAt = Creeper.ExplodedAt;
	C.ExplodedAtTime = GasLabClock::Now();
	C.Radius = AGasLabCreeper::Radius;
	if (Creeper.bExploded && Mode == EGasLabMode::Area)
	{
		C.bCarriesEffects = true;
		if (Creeper.BlastSpec.IsValid()) C.Blast = FGasLabWireSpec::From(*Creeper.BlastSpec.Data);
		if (Creeper.BurnSpec.IsValid()) C.Burn = FGasLabWireSpec::From(*Creeper.BurnSpec.Data);
	}
	for (const FGasLabPeer& P : Peers)
	{
		if (P.Region == Region) continue;
		Send(P.Region, EGasLabMsg::Creeper, [&C](FArchive& Ar) { Ar << C; });
	}
}

void UGasLabBorder::HearCreeper(int32 From, FGasLabWireCreeper& C)
{
	AGasLabCreeper* Ghost = nullptr;
	for (TActorIterator<AGasLabCreeper> It(GetWorld()); It; ++It)
	{
		if (It->bGhost && It->Id == C.Id) { Ghost = *It; break; }
	}
	if (!Ghost)
	{
		if (C.bExploded && C.ExplodedAtTime < GasLabClock::Now() - 2.0) return;
		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Params.bDeferConstruction = true;
		Ghost = GetWorld()->SpawnActor<AGasLabCreeper>(AGasLabCreeper::StaticClass(), C.Origin, FRotator::ZeroRotator, Params);
		if (!Ghost) return;
		Ghost->Id = C.Id;
		Ghost->ThrowerId = C.ThrowerId;
		Ghost->OwnerRegion = C.OwnerRegion;
		Ghost->Origin = C.Origin;
		Ghost->Velocity = C.Velocity;
		Ghost->LaunchAt = C.LaunchAt;
		Ghost->bGhost = true;
		Ghost->FinishSpawning(FTransform(C.Origin));
		UE_LOG(LogTemp, Log, TEXT("GASLAB ghost-creeper creeper=%s owner=%d lag=%.1fms at=%.3f"), *C.Id, C.OwnerRegion, (GasLabClock::Now() - C.LaunchAt) * 1000.0, GasLabClock::Now());
	}
	if (C.bExploded)
	{
		if (C.bCarriesEffects)
		{
			Ghost->GhostBlast = C.Blast;
			Ghost->GhostBurn = C.Burn;
		}
		Ghost->ExplodedElsewhere(C.ExplodedAt, C.ExplodedAtTime);
	}
}

// ── handoff ──────────────────────────────────────────────────────────────────────────────────────

void UGasLabBorder::CheckCrossings()
{
	TArray<TPair<AGasLabCharacter*, int32>> Crossing;
	for (TActorIterator<AGasLabCharacter> It(GetWorld()); It; ++It)
	{
		AGasLabCharacter* C = *It;
		UGasLabAbilitySystemComponent* ASC = C->GetLabASC();
		if (C->IsGhost() || !ASC || ASC->bLeaving || C->IsActorBeingDestroyed()) continue;
		const FVector P = C->GetActorLocation();
		const int32 Now = RegionAt(P);
		// 40 cm of hysteresis, so a player standing on the line is not thrown back and forth.
		if (Now == Region || DistanceToRegion(P, Region) < 40.f) continue;
		if (!Link->IsConnected(Now) && Link->Name() == TEXT("direct")) continue;
		if (AddressOf(Now).IsEmpty()) continue;
		Crossing.Add({ C, Now });
	}
	for (const TPair<AGasLabCharacter*, int32>& X : Crossing) HandOff(*X.Key, X.Value);
}

void UGasLabBorder::HandOff(AGasLabCharacter& Who, int32 ToRegion)
{
	UGasLabAbilitySystemComponent* ASC = Who.GetLabASC();
	if (!ASC || ASC->bLeaving) return;
	ASC->bLeaving = true;
	// An ability halfway through stays here: its prediction belongs to this connection. None starts from now on.
	ASC->AddLooseGameplayTag(HandoffTag());
	ASC->CancelAllAbilities();

	FGasLabWirePlayer P;
	P.Id = Who.GetPlayerId();
	P.Name = Who.GetPlayerState() ? Who.GetPlayerState()->GetPlayerName() : P.Id;
	P.bBot = !Who.IsPlayerControlled();
	P.FromRegion = Region;
	P.Location = Who.GetActorLocation();
	P.Velocity = Who.GetVelocity();
	P.Yaw = Who.GetActorRotation().Yaw;
	P.SentAt = GasLabClock::Now();
	ASC->CaptureState(P);
	Departed.Add(P.Id, ToRegion);

	TArray<uint8> Probe;
	FMemoryWriter Size(Probe);
	Size << P;
	Send(ToRegion, EGasLabMsg::Handoff, [&P](FArchive& Ar) { Ar << P; });
	UE_LOG(LogTemp, Log, TEXT("GASLAB handoff-send id=%s bot=%d to=%d effects=%d bytes=%d hp=%.2f at=%.3f"),
		*P.Id, P.bBot ? 1 : 0, ToRegion, P.Effects.Num(), Probe.Num(), ASC->GetNumericAttribute(UGasLabAttributeSet::GetHealthAttribute()), P.SentAt);

	if (P.bBot)
	{
		BotsLeaving.Add(P.Id, FPlatformTime::Seconds());
		return;
	}
	if (AGasLabPlayerController* PC = Cast<AGasLabPlayerController>(Who.GetController()))
	{
		PC->ClientGasCross(AddressOf(ToRegion), P.Id, ToRegion);
	}
}

void UGasLabBorder::HearHandoff(int32 From, FGasLabWirePlayer& P, int32 Bytes)
{
	DropGhost(P.Id);
	const double Now = GasLabClock::Now();
	UE_LOG(LogTemp, Log, TEXT("GASLAB handoff-recv id=%s bot=%d from=%d effects=%d bytes=%d transit=%.1fms at=%.3f"),
		*P.Id, P.bBot ? 1 : 0, From, P.Effects.Num(), Bytes, (Now - P.SentAt) * 1000.0, Now);
	Departed.Remove(P.Id);
	FString Id = P.Id;
	double Stamp = P.SentAt;
	Send(From, EGasLabMsg::HandoffAck, [&](FArchive& Ar) { Ar << Id << Stamp; });
	if (P.bBot)
	{
		if (AGasLabGameMode* GM = GetWorld()->GetAuthGameMode<AGasLabGameMode>())
		{
			GM->SpawnBot(P.Id, P.Name, P.Location, P.Yaw, &P);
		}
		return;
	}
	Arrivals.Add(P.Id, P);
	ArrivalHeard.Add(P.Id, FPlatformTime::Seconds());
	if (AGasLabGameMode* GM = GetWorld()->GetAuthGameMode<AGasLabGameMode>()) GM->ArrivalReady(P.Id);
}

bool UGasLabBorder::TakeArrival(const FString& Id, FGasLabWirePlayer& Out)
{
	if (!Arrivals.RemoveAndCopyValue(Id, Out)) return false;
	ArrivalHeard.Remove(Id);
	return true;
}

void UGasLabBorder::Restore(UGasLabAbilitySystemComponent& ASC, const FGasLabWirePlayer& State)
{
	FGasLabRestoreReport Report;
	ASC.RestoreState(State, Report);
	UE_LOG(LogTemp, Log, TEXT("GASLAB handoff-restore id=%s from=%d effects=%d/%d transit=%.1fms enderr=[%s] phaseerr=[%s] hp=%.2f burnleft=%.3f stacks=%d nexttick=%.3f cooldown=%.3f clock=%s at=%.3f"),
		*State.Id, State.FromRegion, Report.Restored, Report.Effects, Report.Transit * 1000.f, *JoinFloats(Report.EndErrors), *JoinFloats(Report.PhaseErrors),
		ASC.GetNumericAttribute(UGasLabAttributeSet::GetHealthAttribute()), ASC.TimeLeft(FGameplayTag::RequestGameplayTag(TEXT("Effect.Burning"))),
		ASC.Stacks(FGameplayTag::RequestGameplayTag(TEXT("Effect.Burning"))), ASC.NextTickIn(FGameplayTag::RequestGameplayTag(TEXT("Effect.Burning"))),
		ASC.TimeLeft(FGameplayTag::RequestGameplayTag(TEXT("Cooldown.Throw"))), GasLabClock::IsShared() ? TEXT("shared") : TEXT("local"), GasLabClock::Now());
	if (TArray<FGasLabWireSpec>* Waiting = EffectsWaiting.Find(State.Id))
	{
		for (FGasLabWireSpec& Wire : *Waiting)
		{
			if (TSharedPtr<FGameplayEffectSpec> Spec = Wire.Build(nullptr)) ASC.ApplyHere(*Spec);
		}
		UE_LOG(LogTemp, Log, TEXT("GASLAB effects-caught-up id=%s n=%d at=%.3f"), *State.Id, Waiting->Num(), GasLabClock::Now());
		EffectsWaiting.Remove(State.Id);
	}
}

void UGasLabBorder::HearAck(int32 From, const FString& Id, double SentAt)
{
	UE_LOG(LogTemp, Log, TEXT("GASLAB handoff-ack id=%s from=%d rtt=%.1fms at=%.3f"), *Id, From, (GasLabClock::Now() - SentAt) * 1000.0, GasLabClock::Now());
	if (BotsLeaving.Remove(Id) > 0)
	{
		if (AGasLabCharacter* C = FindCharacter(Id, false))
		{
			AController* Controller = C->GetController();
			if (APlayerState* PS = C->GetPlayerState()) PS->Destroy();
			C->Destroy();
			if (Controller) Controller->Destroy();
		}
	}
}

// ── the wire ─────────────────────────────────────────────────────────────────────────────────────

void UGasLabBorder::Hear(int32 From, const TArray<uint8>& Bytes)
{
	FMemoryReader Reader(Bytes);
	uint8 T = 0;
	Reader << T;
	switch (EGasLabMsg(T))
	{
	case EGasLabMsg::Ghosts: { TArray<FGasLabWireGhost> G; Reader << G; HearGhosts(From, G); break; }
	case EGasLabMsg::Creeper: { FGasLabWireCreeper C; Reader << C; HearCreeper(From, C); break; }
	case EGasLabMsg::Effect: { FString Id; FGasLabWireSpec S; double At = 0; Reader << Id << S << At; HearEffect(From, Id, S, At); break; }
	case EGasLabMsg::Credit: { FGasLabWireCredit C; Reader << C; HearCredit(From, C); break; }
	case EGasLabMsg::Handoff: { FGasLabWirePlayer P; Reader << P; HearHandoff(From, P, Bytes.Num()); break; }
	case EGasLabMsg::HandoffAck: { FString Id; double At = 0; Reader << Id << At; HearAck(From, Id, At); break; }
	case EGasLabMsg::Probe:
	{
		double At = 0;
		Reader << At;
		Send(From, EGasLabMsg::ProbeEcho, [&At](FArchive& Ar) { Ar << At; });
		break;
	}
	case EGasLabMsg::Start: { double At = 0; Reader << At; ScenarioStart = At; break; }
	case EGasLabMsg::ProbeEcho:
	{
		double At = 0;
		Reader << At;
		UE_LOG(LogTemp, Log, TEXT("GASLAB probe to=%d rtt=%.2fms link=%s at=%.3f"), From, (GasLabClock::Now() - At) * 1000.0, *Link->Name(), GasLabClock::Now());
		break;
	}
	default: break;
	}
}
