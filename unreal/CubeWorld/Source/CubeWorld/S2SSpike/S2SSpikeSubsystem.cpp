#include "S2SSpikeSubsystem.h"
#include "S2SActors.h"
#include "S2SBeacon.h"
#include "S2SNetDriver.h"
#include "S2STypes.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "GameplayCueManager.h"
#include "Engine/Engine.h"
#include "Engine/NetConnection.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "Iris/ReplicationSystem/ReplicationSystem.h"
#include "Iris/ReplicationSystem/ObjectReplicationBridge.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Net/Core/Connection/NetEnums.h"
#include "Net/Iris/ReplicationSystem/ReplicationSystemUtil.h"

namespace
{
	const FName S2SDefinitionName(S2S_NETDRIVER_DEFINITION);

	const TCHAR* CueEventName(EGameplayCueEvent::Type Event)
	{
		switch (Event)
		{
		case EGameplayCueEvent::OnActive: return TEXT("OnActive");
		case EGameplayCueEvent::WhileActive: return TEXT("WhileActive");
		case EGameplayCueEvent::Executed: return TEXT("Executed");
		case EGameplayCueEvent::Removed: return TEXT("Removed");
		default: return TEXT("?");
		}
	}

	/** The S2S driver class and its Iris config, added at runtime so nothing changes for a normal run of the game. */
	void RegisterS2SDriverDefinition()
	{
		if (!GEngine) return;
		const bool bKnown = GEngine->NetDriverDefinitions.ContainsByPredicate([](const FNetDriverDefinition& Def) { return Def.DefName == S2SDefinitionName; });
		if (bKnown) return;

		// Same as the ini lines:
		//   [/Script/Engine.Engine]
		//   +NetDriverDefinitions=(DefName="S2SNetDriver",DriverClassName="/Script/CubeWorld.S2SNetDriver",DriverClassNameFallback="/Script/CubeWorld.S2SNetDriver")
		//   +IrisNetDriverConfigs=(NetDriverDefinition=S2SNetDriver,bCanUseIris=true)
		FNetDriverDefinition Def;
		Def.DefName = S2SDefinitionName;
		Def.DriverClassName = FName(TEXT("/Script/CubeWorld.S2SNetDriver"));
		Def.DriverClassNameFallback = Def.DriverClassName;
		GEngine->NetDriverDefinitions.Add(Def);

		FIrisNetDriverConfig Iris;
		Iris.NetDriverDefinition = S2SDefinitionName;
		Iris.bCanUseIris = true;
		GEngine->IrisNetDriverConfigs.Add(Iris);
	}

	FString DescribeDriver(const UNetDriver* Driver)
	{
		if (!Driver) return TEXT("none");
		return FString::Printf(TEXT("%s (class %s, %s, iris=%d, replication system id=%d, MaxClientRate=%d, NetServerMaxTickRate=%d, channel definitions=%d, any-role=%d, swap-roles=%d, proxy=%d)"),
			*Driver->NetDriverName.ToString(), *Driver->GetClass()->GetName(), Driver->IsServer() ? TEXT("server") : TEXT("client"),
			Driver->IsUsingIrisReplication() ? 1 : 0, Driver->GetReplicationSystem() ? (int32)Driver->GetReplicationSystem()->GetId() : -1,
			Driver->MaxClientRate, Driver->GetNetServerMaxTickRate(), Driver->ChannelDefinitions.Num(),
			Driver->IsReplicationOfActorsWithAnyRoleEnabled() ? 1 : 0, Driver->IsRoleSwappingOnAllActorsEnabled() ? 1 : 0,
			(int32)Driver->GetProxyType());
	}
}

static FString DescribeHandles(const AActor* Actor);

US2SSpikeSubsystem::US2SSpikeSubsystem() = default;
US2SSpikeSubsystem::~US2SSpikeSubsystem() = default;

bool US2SSpikeSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	static const bool bSpike = FParse::Param(FCommandLine::Get(), TEXT("s2sspike"));
	return bSpike && Super::ShouldCreateSubsystem(Outer);
}

bool US2SSpikeSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

TStatId US2SSpikeSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(US2SSpikeSubsystem, STATGROUP_Tickables);
}

void US2SSpikeSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	bServer = IsRunningDedicatedServer();
	RegisterS2SDriverDefinition();

	FString Relay;
	if (FParse::Value(FCommandLine::Get(), TEXT("-s2srelay="), Relay, false))
	{
		TArray<FString> Tokens;
		Relay.ParseIntoArray(Tokens, TEXT(","));
		bRelayAnyRole = Tokens.Contains(TEXT("anyrole"));
		bRelaySwap = Tokens.Contains(TEXT("swap"));
		bRelayFrontend = Tokens.Contains(TEXT("frontend"));
		bRelayLocalSwap = Tokens.Contains(TEXT("localswap"));
	}
	// The relay switches are read when a driver creates its replication system (UNetDriver::InitBase), so they go on the
	// game driver as soon as UWorld::Listen creates it.
	NetDriverCreatedHandle = FWorldDelegates::OnNetDriverCreated.AddUObject(this, &US2SSpikeSubsystem::HandleNetDriverCreated);

	if (bServer)
	{
		// GameplayCues are not routed at all on a dedicated server (UGameplayCueManager::ShouldSuppressGameplayCues); the
		// spike routes them so it can see which ones reach a copy.
		if (IConsoleVariable* RunOnServer = IConsoleManager::Get().FindConsoleVariable(TEXT("AbilitySystem.GameplayCue.RunOnDedicatedServer")))
		{
			RunOnServer->Set(1, ECVF_SetByCode);
		}
	}
	UE_LOG(LogS2S, Display, TEXT("[S2S %s] spike up in world %s (server=%d, relay: anyrole=%d swap=%d frontend=%d localswap=%d)"), *S2SStats::Who(),
		*GetWorld()->GetName(), bServer ? 1 : 0, bRelayAnyRole ? 1 : 0, bRelaySwap ? 1 : 0, bRelayFrontend ? 1 : 0, bRelayLocalSwap ? 1 : 0);
}

void US2SSpikeSubsystem::Deinitialize()
{
	if (!bFinished && StartedAt > 0.0)
	{
		LogTraffic(true);
		S2SStats::LogLatency(TEXT("final"), true);
	}
	FWorldDelegates::OnNetDriverCreated.Remove(NetDriverCreatedHandle);
	if (CueRouteHandle.IsValid())
	{
		if (UGameplayCueManager* Cues = UAbilitySystemGlobals::Get().GetGameplayCueManager()) Cues->OnGameplayCueRouted().Remove(CueRouteHandle);
	}
	// The world's cleanup may have destroyed the driver already; UPendingNetGame ensures its driver is cleared either way.
	if (PeerLink) PeerLink->NetDriver = nullptr;
	if (UWorld* World = GetWorld(); World && GEngine && !S2SDriverName.IsNone() && GEngine->FindNamedNetDriver(World, S2SDriverName))
	{
		GEngine->DestroyNamedNetDriver(World, S2SDriverName);
	}
	S2SDriver = nullptr;
	PeerLink = nullptr;
	HostNotify.Reset();
	Super::Deinitialize();
}

void US2SSpikeSubsystem::HandleNetDriverCreated(UWorld* InWorld, UNetDriver* Driver)
{
	if (InWorld != GetWorld() || !Driver || !bServer || Driver->NetDriverName != NAME_GameNetDriver) return;
	if (bRelayAnyRole) Driver->EnableReplicationOfActorsWithAnyRole(true);
	if (bRelaySwap) Driver->EnableRoleSwappingOfAllActors(true);
	if (bRelayFrontend) Driver->SetProxyType(UE::Net::EProxyType::Frontend);
	if (bRelayAnyRole || bRelaySwap || bRelayFrontend)
	{
		UE_LOG(LogS2S, Display, TEXT("[S2S %s] relay switches set on the game driver before it listens: anyrole=%d swap=%d frontend=%d"),
			*S2SStats::Who(), bRelayAnyRole ? 1 : 0, bRelaySwap ? 1 : 0, bRelayFrontend ? 1 : 0);
	}
}

void US2SSpikeSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	StartedAt = FPlatformTime::Seconds();
	NextReportAt = StartedAt + 5.0;
	double QuitAfter = 0.0;
	if (FParse::Value(FCommandLine::Get(), TEXT("-s2squitafter="), QuitAfter) && QuitAfter > 0.0) QuitAt = StartedAt + QuitAfter;

	if (UGameplayCueManager* Cues = UAbilitySystemGlobals::Get().GetGameplayCueManager())
	{
		CueRouteHandle = Cues->OnGameplayCueRouted().AddUObject(this, &US2SSpikeSubsystem::HandleCueRouted);
	}

	UE_LOG(LogS2S, Display, TEXT("[S2S %s] world %s begins play; game driver: %s"), *S2SStats::Who(), *InWorld.GetName(), *DescribeDriver(InWorld.GetNetDriver()));
	if (!bServer) return;

	int32 HostPort = 0;
	if (FParse::Value(FCommandLine::Get(), TEXT("-s2shost="), HostPort) && HostPort > 0)
	{
		StartHost(HostPort);
		SpawnHostActors();
	}
	FString Peer;
	if (FParse::Value(FCommandLine::Get(), TEXT("-s2speer="), Peer) && !Peer.IsEmpty())
	{
		PeerAddress = Peer;
		PeerConnectAt = StartedAt + 1.0;
		SpawnPeerActors();
	}
	if (FParse::Value(FCommandLine::Get(), TEXT("-s2sbeaconhost="), HostPort) && HostPort > 0)
	{
		StartBeaconHost(HostPort);
		SpawnHostActors();
	}
	if (FParse::Value(FCommandLine::Get(), TEXT("-s2sbeaconpeer="), Peer) && !Peer.IsEmpty())
	{
		PeerAddress = Peer;
		bBeaconPeer = true;
		PeerConnectAt = StartedAt + 1.0;
		SpawnPeerActors();
	}
}

void US2SSpikeSubsystem::StartBeaconHost(int32 Port)
{
	UWorld* World = GetWorld();
	AS2SBeaconHost* Host = World->SpawnActor<AS2SBeaconHost>();
	Host->ListenPort = Port;
	if (!Host->InitHost())
	{
		UE_LOG(LogS2S, Error, TEXT("[S2S %s] beacon host: InitHost on %d failed"), *S2SStats::Who(), Port);
		return;
	}
	AS2SBeaconHostObject* HostObject = World->SpawnActor<AS2SBeaconHostObject>();
	Host->RegisterHost(HostObject);
	Host->PauseBeaconRequests(false);
	BeaconHost = Host;
	BeaconHostObject = HostObject;
	S2SDriver = Host->GetNetDriver();
	UE_LOG(LogS2S, Display, TEXT("[S2S %s] beacon host listening on UDP %d: %s"), *S2SStats::Who(), Host->GetListenPort(), *DescribeDriver(S2SDriver));
}

void US2SSpikeSubsystem::StartBeaconPeer(const FString& Address)
{
	UWorld* World = GetWorld();
	AS2SBeaconClient* Client = World->SpawnActor<AS2SBeaconClient>();
	FURL URL(nullptr, *Address, TRAVEL_Absolute);
	BeaconClient = Client;
	if (!Client->InitClient(URL))
	{
		UE_LOG(LogS2S, Error, TEXT("[S2S %s] beacon peer: InitClient to %s failed"), *S2SStats::Who(), *Address);
		return;
	}
	S2SDriver = Client->GetNetDriver();
	UE_LOG(LogS2S, Display, TEXT("[S2S %s] beacon peer connecting to %s: %s"), *S2SStats::Who(), *URL.ToString(), *DescribeDriver(S2SDriver));
}

void US2SSpikeSubsystem::StartHost(int32 Port)
{
	UWorld* World = GetWorld();
	S2SDriverName = FName(TEXT("S2SHost"));
	if (!GEngine->CreateNamedNetDriver(World, S2SDriverName, S2SDefinitionName))
	{
		UE_LOG(LogS2S, Error, TEXT("[S2S %s] host: could not create the S2S driver"), *S2SStats::Who());
		return;
	}
	S2SDriver = GEngine->FindNamedNetDriver(World, S2SDriverName);
	// SetWorld before InitListen: SetWorld makes the world the driver's notify, InitListen replaces it with ours.
	S2SDriver->SetWorld(World);
	HostNotify = MakeUnique<FS2SHostNotify>(World);
	FURL ListenURL;
	ListenURL.Port = Port;
	FString Error;
	if (!S2SDriver->InitListen(HostNotify.Get(), ListenURL, false, Error))
	{
		UE_LOG(LogS2S, Error, TEXT("[S2S %s] host: listen on %d failed: %s"), *S2SStats::Who(), Port, *Error);
		GEngine->DestroyNamedNetDriver(World, S2SDriverName);
		S2SDriver = nullptr;
		return;
	}
	UE_LOG(LogS2S, Display, TEXT("[S2S %s] host: S2S driver listening on UDP %d: %s"), *S2SStats::Who(), ListenURL.Port, *DescribeDriver(S2SDriver));
}

void US2SSpikeSubsystem::StartPeer(const FString& Address)
{
	UWorld* World = GetWorld();
	S2SDriverName = FName(TEXT("S2SPeer"));
	if (!GEngine->CreateNamedNetDriver(World, S2SDriverName, S2SDefinitionName))
	{
		UE_LOG(LogS2S, Error, TEXT("[S2S %s] peer: could not create the S2S driver"), *S2SStats::Who());
		PeerAddress.Reset();
		return;
	}
	S2SDriver = GEngine->FindNamedNetDriver(World, S2SDriverName);
	if (bRelayFrontend) S2SDriver->SetProxyType(UE::Net::EProxyType::Backend);
	S2SDriver->SetWorld(World);

	FURL URL(nullptr, *Address, TRAVEL_Absolute);
	URL.AddOption(*FString::Printf(TEXT("S2SPeer=%s"), *S2SStats::Who()));
	PeerLink = NewObject<US2SPeerLink>(this);
	PeerLink->Initialize(URL);
	PeerLink->InitNetDriver(S2SDriver);
	// UPendingNetGame::InitNetDriver hands the driver its own notify; keep it (as MultiServer's proxy does).
	S2SDriver->Notify = PeerLink;
	if (!PeerLink->ConnectionError.IsEmpty())
	{
		UE_LOG(LogS2S, Error, TEXT("[S2S %s] peer: connect to %s failed: %s"), *S2SStats::Who(), *Address, *PeerLink->ConnectionError);
		return;
	}
	UE_LOG(LogS2S, Display, TEXT("[S2S %s] peer: S2S driver connecting to %s: %s"), *S2SStats::Who(), *URL.ToString(), *DescribeDriver(S2SDriver));
}

void US2SSpikeSubsystem::SpawnHostActors()
{
	UWorld* World = GetWorld();
	int32 Count = 1;
	FParse::Value(FCommandLine::Get(), TEXT("-s2sprojectiles="), Count);
	Count = FMath::Clamp(Count, 0, 2000);

	auto SpawnProjectile = [this, World](const FVector& At, bool bMirror, const FString& Origin)
	{
		const FTransform Where(At);
		AS2SProjectile* Projectile = World->SpawnActorDeferred<AS2SProjectile>(AS2SProjectile::StaticClass(), Where);
		Projectile->bMirrorToPeers = bMirror;
		Projectile->Origin = Origin;
		Projectile->Payload.EffectClass = FSoftClassPath(US2SDamageEffect::StaticClass());
		Projectile->Payload.Level = 2.f;
		FS2SSetByCaller& Damage = Projectile->Payload.SetByCallers.AddDefaulted_GetRef();
		Damage.Tag = TAG_S2S_SetByCaller_Damage;
		Damage.Magnitude = -25.f;
		Projectile->Payload.InstigatorId = FString::Printf(TEXT("%s:player-17"), *S2SStats::Who());
		Projectile->FinishSpawning(Where);
	};
	for (int32 I = 0; I < Count; ++I)
	{
		// Spread along the lane so they cross the border at different times; 2 m apart sideways.
		const double X = -20000.0 + (Count > 1 ? 40000.0 * I / Count : 10000.0);
		SpawnProjectile(FVector(X, 200.0 * I, 100.0), true, S2SStats::Who());
	}
	// A control that must stay on A: not opted in.
	SpawnProjectile(FVector(-15000.0, -500.0, 100.0), false, S2SStats::Who() + TEXT("-local"));

	const FTransform AscAt(FVector(-500.0, 0.0, 100.0));
	AS2SAscActor* Asc = World->SpawnActorDeferred<AS2SAscActor>(AS2SAscActor::StaticClass(), AscAt);
	Asc->Origin = S2SStats::Who();
	Asc->FinishSpawning(AscAt);

	UE_LOG(LogS2S, Display, TEXT("[S2S %s] host: spawned %d mirrored projectiles, 1 local-only projectile, 1 mirrored ASC actor"), *S2SStats::Who(), Count);
}

void US2SSpikeSubsystem::SpawnPeerActors()
{
	UWorld* World = GetWorld();
	// B's own ASC actor: the target of the effects the copies carry, and the positive control of the GAS checks.
	const FTransform TargetAt(FVector(1000.0, 0.0, 100.0));
	LocalTarget = World->SpawnActorDeferred<AS2SAscActor>(AS2SAscActor::StaticClass(), TargetAt);
	LocalTarget->Origin = S2SStats::Who();
	LocalTarget->bMirrorToPeers = false;
	LocalTarget->bRunAuthorityTimers = false;
	LocalTarget->FinishSpawning(TargetAt);

	// B's own projectile: what B's clients must keep seeing whatever the relay switches do to the copies.
	const FTransform ProjectileAt(FVector(5000.0, 500.0, 100.0));
	AS2SProjectile* Projectile = World->SpawnActorDeferred<AS2SProjectile>(AS2SProjectile::StaticClass(), ProjectileAt);
	Projectile->bMirrorToPeers = false;
	Projectile->Origin = S2SStats::Who();
	Projectile->Payload.InstigatorId = FString::Printf(TEXT("%s:player-3"), *S2SStats::Who());
	Projectile->FinishSpawning(ProjectileAt);
	UE_LOG(LogS2S, Display, TEXT("[S2S %s] peer: spawned its own target ASC actor and its own projectile"), *S2SStats::Who());
}

void US2SSpikeSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (StartedAt <= 0.0 || bFinished) return;
	const double Now = FPlatformTime::Seconds();

	if (!PeerAddress.IsEmpty() && !PeerLink && !BeaconClient && Now >= PeerConnectAt)
	{
		if (bBeaconPeer) StartBeaconPeer(PeerAddress);
		else StartPeer(PeerAddress);
	}

	if (bServer && bRelayLocalSwap) LocalSwapCopies();

	if (bServer && LocalTarget)
	{
		CheckBorderHits();

		AS2SAscActor* Copy = nullptr;
		for (TActorIterator<AS2SAscActor> It(GetWorld()); It; ++It)
		{
			if (It->Origin != S2SStats::Who()) { Copy = *It; break; }
		}
		if (Copy && CopySeenAt <= 0.0) CopySeenAt = Now;
		if (Copy && !bGasChecked && Now - CopySeenAt >= 3.0) RunGasChecks(Copy);
		if (bGasChecked && GasCheckedAt > 0.0 && Now - GasCheckedAt >= 2.0)
		{
			GasCheckedAt = 0.0;
			int32 FromB = 0, FromBLocal = 0;
			for (const TPair<FString, int32>& Route : CueRoutes)
			{
				if (Route.Key.StartsWith(TAG_S2S_Cue_FromBLocal.GetTag().ToString() + TEXT(" "))) FromBLocal += Route.Value;
				else if (Route.Key.StartsWith(TAG_S2S_Cue_FromB.GetTag().ToString() + TEXT(" "))) FromB += Route.Value;
			}
			const FString Line = FString::Printf(TEXT("6b cue routes 2 s later: GameplayCue.S2S.FromB executed on the copy routed %d times; GameplayCue.S2S.FromBLocal executed on B's own ASC routed %d times; copy now: %s"),
				FromB, FromBLocal, *Copy->Describe());
			GasResults.Add(Line);
			UE_LOG(LogS2S, Display, TEXT("[S2S %s] GAS CHECK %s"), *S2SStats::Who(), *Line);
		}
	}

	if (Now >= NextReportAt)
	{
		NextReportAt = Now + 5.0;
		LogTraffic(false);
		S2SStats::LogLatency(TEXT("last 5 s"), false);
		LogInventory();
		LogCues();
	}
	if (QuitAt > 0.0 && Now >= QuitAt) Finish();
}

void US2SSpikeSubsystem::LocalSwapCopies()
{
	// A relay without an engine change: the copy's local roles become Authority/SimulatedProxy, so B's game driver
	// accepts it (EngineReplicationBridge's role gate) and its clients receive SimulatedProxy. B's own game code now sees
	// HasAuthority() == true on an actor A owns.
	for (TActorIterator<AActor> It(GetWorld()); It; ++It)
	{
		AS2SProjectile* Projectile = Cast<AS2SProjectile>(*It);
		AS2SAscActor* Asc = Cast<AS2SAscActor>(*It);
		bool* Flag = Projectile ? &Projectile->bS2SCopy : Asc ? &Asc->bS2SCopy : nullptr;
		if (!Flag || *Flag || It->GetLocalRole() != ROLE_SimulatedProxy || !It->HasActorBegunPlay()) continue;
		*Flag = true;
		It->SwapRoles();
		UE::Net::FReplicationSystemUtil::StartReplicatingActor(*It);
		UE_LOG(LogS2S, Display, TEXT("[S2S %s] localswap: %s now role=%s remote=%s; handles:%s"), *S2SStats::Who(), *It->GetName(),
			*S2SStats::RoleName(It->GetLocalRole()), *S2SStats::RoleName(It->GetRemoteRole()), *DescribeHandles(*It));
	}
}

void US2SSpikeSubsystem::CheckBorderHits()
{
	for (TActorIterator<AS2SProjectile> It(GetWorld()); It; ++It)
	{
		AS2SProjectile* Projectile = *It;
		if (Projectile->Origin == S2SStats::Who()) continue;
		const double X = Projectile->GetActorLocation().X;
		const double* Prev = LastX.Find(Projectile);
		if (Prev && *Prev < 0.0 && X >= 0.0 && X - *Prev < Projectile->HalfLane)
		{
			// The copy crossed into B's half: B is the authority for its own target, so it rebuilds the effect from the
			// payload and applies it with its own ASC as the source (A's thrower has no object here).
			BorderHits++;
			UAbilitySystemComponent* Asc = LocalTarget->AbilitySystem;
			const FS2SEffectPayload& Payload = Projectile->Payload;
			UClass* EffectClass = Payload.EffectClass.TryLoadClass<UGameplayEffect>();
			const float Before = Asc->GetNumericAttribute(US2SAttributeSet::GetHealthAttribute());
			bool bApplied = false;
			if (EffectClass)
			{
				FGameplayEffectSpecHandle Spec = Asc->MakeOutgoingSpec(EffectClass, Payload.Level, Asc->MakeEffectContext());
				for (const FS2SSetByCaller& Value : Payload.SetByCallers) Spec.Data->SetSetByCallerMagnitude(Value.Tag, Value.Magnitude);
				bApplied = Asc->ApplyGameplayEffectSpecToSelf(*Spec.Data).WasSuccessfullyApplied();
			}
			const float After = Asc->GetNumericAttribute(US2SAttributeSet::GetHealthAttribute());
			if (BorderHits <= 5 || BorderHits % 10 == 0)
			{
				UE_LOG(LogS2S, Display, TEXT("[S2S %s] HIT #%d: copy %s (origin %s, instigator %s) crossed into B at x=%.0f; B rebuilt %s level %.0f with %d SetByCaller from the replicated payload and applied it to its own target: applied=%d, Health %.1f -> %.1f"),
					*S2SStats::Who(), BorderHits, *Projectile->GetName(), *Projectile->Origin, *Payload.InstigatorId, X,
					EffectClass ? *EffectClass->GetName() : TEXT("<class not found>"), Payload.Level, Payload.SetByCallers.Num(),
					bApplied ? 1 : 0, Before, After);
			}
		}
		LastX.Add(Projectile, X);
	}
}

void US2SSpikeSubsystem::RunGasChecks(AS2SAscActor* Copy)
{
	bGasChecked = true;
	GasCheckedAt = FPlatformTime::Seconds();
	UAbilitySystemComponent* Remote = Copy->AbilitySystem;
	UAbilitySystemComponent* Own = LocalTarget->AbilitySystem;
	const FGameplayAttribute Health = US2SAttributeSet::GetHealthAttribute();
	auto Line = [this](const FString& Text)
	{
		GasResults.Add(Text);
		UE_LOG(LogS2S, Display, TEXT("[S2S %s] GAS CHECK %s"), *S2SStats::Who(), *Text);
	};

	Line(FString::Printf(TEXT("1 read the copy: %s; S2S.State.Burning=%d, GameplayCue.S2S.Loop tag=%d, IsOwnerActorAuthoritative=%d, ActivatableAbilities=%d"),
		*Copy->Describe(), Remote->HasMatchingGameplayTag(TAG_S2S_State_Burning) ? 1 : 0, Remote->HasMatchingGameplayTag(TAG_S2S_Cue_Loop) ? 1 : 0,
		Remote->IsOwnerActorAuthoritative() ? 1 : 0, Remote->GetActivatableAbilities().Num()));

	{
		const float Before = Remote->GetNumericAttribute(Health);
		FGameplayEffectSpecHandle Spec = Remote->MakeOutgoingSpec(US2SDamageEffect::StaticClass(), 1.f, Remote->MakeEffectContext());
		Spec.Data->SetSetByCallerMagnitude(TAG_S2S_SetByCaller_Damage, -50.f);
		const FActiveGameplayEffectHandle Result = Remote->ApplyGameplayEffectSpecToSelf(*Spec.Data);
		Line(FString::Printf(TEXT("2 ApplyGameplayEffectSpecToSelf(instant -50) on the copy: applied=%d, Health %.1f -> %.1f"),
			Result.WasSuccessfullyApplied() ? 1 : 0, Before, Remote->GetNumericAttribute(Health)));
	}
	{
		const int32 Before = Remote->GetActiveGameplayEffects().GetNumGameplayEffects();
		const FActiveGameplayEffectHandle Result = Remote->ApplyGameplayEffectToSelf(GetDefault<US2SBuffEffect>(), 1.f, Remote->MakeEffectContext());
		Line(FString::Printf(TEXT("3 ApplyGameplayEffectToSelf(infinite +100) on the copy: handle valid=%d, active effects %d -> %d"),
			Result.IsValid() ? 1 : 0, Before, Remote->GetActiveGameplayEffects().GetNumGameplayEffects()));
	}
	{
		const float Before = Own->GetNumericAttribute(Health);
		FGameplayEffectSpecHandle Spec = Remote->MakeOutgoingSpec(US2SDamageEffect::StaticClass(), 1.f, Remote->MakeEffectContext());
		Spec.Data->SetSetByCallerMagnitude(TAG_S2S_SetByCaller_Damage, -50.f);
		const FActiveGameplayEffectHandle Result = Remote->ApplyGameplayEffectSpecToTarget(*Spec.Data, Own);
		Line(FString::Printf(TEXT("4 ApplyGameplayEffectSpecToTarget(instant -50) from the copy to B's own ASC: applied=%d, B's Health %.1f -> %.1f"),
			Result.WasSuccessfullyApplied() ? 1 : 0, Before, Own->GetNumericAttribute(Health)));
	}
	{
		const float Before = Remote->GetNumericAttribute(Health);
		FGameplayEffectSpecHandle Spec = Own->MakeOutgoingSpec(US2SDamageEffect::StaticClass(), 1.f, Own->MakeEffectContext());
		Spec.Data->SetSetByCallerMagnitude(TAG_S2S_SetByCaller_Damage, -50.f);
		const FActiveGameplayEffectHandle Result = Own->ApplyGameplayEffectSpecToTarget(*Spec.Data, Remote);
		Line(FString::Printf(TEXT("5 ApplyGameplayEffectSpecToTarget(instant -50) from B's own ASC to the copy: applied=%d, copy's Health %.1f -> %.1f"),
			Result.WasSuccessfullyApplied() ? 1 : 0, Before, Remote->GetNumericAttribute(Health)));
	}
	Remote->ExecuteGameplayCue(TAG_S2S_Cue_FromB);
	Own->ExecuteGameplayCue(TAG_S2S_Cue_FromBLocal);
	Line(TEXT("6 executed GameplayCue.S2S.FromB on the copy and GameplayCue.S2S.FromBLocal on B's own ASC (routes counted 2 s later)"));
}

void US2SSpikeSubsystem::HandleCueRouted(AActor* Target, FGameplayTag Tag, EGameplayCueEvent::Type Event, const FGameplayCueParameters& Params, EGameplayCueExecutionOptions Options)
{
	FString Origin = TEXT("?");
	if (const AS2SAscActor* Asc = Cast<AS2SAscActor>(Target)) Origin = Asc->Origin;
	const FString Key = FString::Printf(TEXT("%s %s on origin=%s role=%s"), *Tag.ToString(), CueEventName(Event), *Origin,
		Target ? *S2SStats::RoleName(Target->GetLocalRole()) : TEXT("-"));
	int32& Count = CueRoutes.FindOrAdd(Key);
	if (Count++ == 0)
	{
		UE_LOG(LogS2S, Display, TEXT("[S2S %s] CUE ROUTED (first) %s"), *S2SStats::Who(), *Key);
	}
}

void US2SSpikeSubsystem::LogConnection(const FString& Label, UNetConnection* Connection, bool bCumulative)
{
	if (!Connection) return;
	const double Now = FPlatformTime::Seconds();
	FTrafficSample Sample;
	Sample.InBytes = Connection->InTotalBytes;
	Sample.OutBytes = Connection->OutTotalBytes;
	Sample.InPackets = Connection->InTotalPackets;
	Sample.OutPackets = Connection->OutTotalPackets;
	Sample.At = Now;
	if (!FirstTraffic.Contains(Connection)) FirstTraffic.Add(Connection, Sample);
	const FTrafficSample* Prev = bCumulative ? FirstTraffic.Find(Connection) : LastTraffic.Find(Connection);
	const FTrafficSample* First = FirstTraffic.Find(Connection);
	if (Prev && Now - Prev->At > 0.5)
	{
		const double Dt = Now - Prev->At;
		const double Total = FMath::Max(Now - First->At, 0.001);
		UE_LOG(LogS2S, Display, TEXT("[S2S %s] traffic %s %s over %.1f s: in %.0f B/s (%.1f pkt/s), out %.0f B/s (%.1f pkt/s); since first seen (%.0f s): in %.0f B/s, out %.0f B/s, out %.1f B/packet; ping %.1f ms; %s"),
			*S2SStats::Who(), bCumulative ? TEXT("CUMULATIVE") : TEXT("window"), *Label, Dt,
			(Sample.InBytes - Prev->InBytes) / Dt, (Sample.InPackets - Prev->InPackets) / Dt,
			(Sample.OutBytes - Prev->OutBytes) / Dt, (Sample.OutPackets - Prev->OutPackets) / Dt,
			Total, (Sample.InBytes - First->InBytes) / Total, (Sample.OutBytes - First->OutBytes) / Total,
			Sample.OutPackets > First->OutPackets ? double(Sample.OutBytes - First->OutBytes) / double(Sample.OutPackets - First->OutPackets) : 0.0,
			Connection->AvgLag * 1000.0, *Connection->LowLevelGetRemoteAddress(true));
	}
	LastTraffic.Add(Connection, Sample);
}

void US2SSpikeSubsystem::LogTraffic(bool bCumulative)
{
	UWorld* World = GetWorld();
	if (!World) return;
	if (S2SDriver)
	{
		if (S2SDriver->ServerConnection) LogConnection(TEXT("S2S link at the peer (B)"), S2SDriver->ServerConnection, bCumulative);
		for (UNetConnection* Connection : S2SDriver->ClientConnections) LogConnection(TEXT("S2S link at the host (A)"), Connection, bCumulative);
	}
	if (UNetDriver* Game = World->GetNetDriver())
	{
		if (Game->ServerConnection) LogConnection(TEXT("game connection at the client"), Game->ServerConnection, bCumulative);
		for (UNetConnection* Connection : Game->ClientConnections) LogConnection(TEXT("game connection at the server"), Connection, bCumulative);
	}
}

/** In which of this world's Iris replication systems the actor has a NetRefHandle: "GameNetDriver=12 S2SPeer=-". */
static FString DescribeHandles(const AActor* Actor)
{
	FString Out;
	const FWorldContext* Context = GEngine ? GEngine->GetWorldContextFromWorld(Actor->GetWorld()) : nullptr;
	if (!Context) return Out;
	for (const FNamedNetDriver& Named : Context->ActiveNetDrivers)
	{
		const UNetDriver* Driver = Named.NetDriver;
		const UReplicationSystem* System = Driver ? Driver->GetReplicationSystem() : nullptr;
		const UObjectReplicationBridge* Bridge = System ? System->GetReplicationBridge() : nullptr;
		if (!Bridge) continue;
		const UE::Net::FNetRefHandle Handle = Bridge->GetReplicatedRefHandle(Actor);
		Out += FString::Printf(TEXT(" %s(%s)=%s"), *Driver->NetDriverName.ToString(), System->IsServer() ? TEXT("server") : TEXT("client"),
			Handle.IsValid() ? *FString::Printf(TEXT("%llu"), (unsigned long long)Handle.GetId()) : TEXT("none"));
	}
	return Out;
}

void US2SSpikeSubsystem::LogInventory()
{
	UWorld* World = GetWorld();
	if (!World) return;
	TMap<FString, int32> Projectiles;
	for (TActorIterator<AS2SProjectile> It(World); It; ++It)
	{
		Projectiles.FindOrAdd(FString::Printf(TEXT("origin=%s role=%s"), *It->Origin, *S2SStats::RoleName(It->GetLocalRole())))++;
	}
	FString Line;
	for (const TPair<FString, int32>& Pair : Projectiles) Line += FString::Printf(TEXT(" {%s: %d}"), *Pair.Key, Pair.Value);
	// Which replication systems hold each actor (only a few actors, or the line gets long).
	int32 Listed = 0;
	for (TActorIterator<AActor> It(World); It && Listed < 6; ++It)
	{
		if (!It->IsA<AS2SProjectile>() && !It->IsA<AS2SAscActor>()) continue;
		Listed++;
		const FString Origin = It->IsA<AS2SProjectile>() ? Cast<AS2SProjectile>(*It)->Origin : Cast<AS2SAscActor>(*It)->Origin;
		UE_LOG(LogS2S, Display, TEXT("[S2S %s] handles: %s origin=%s role=%s:%s"), *S2SStats::Who(), *It->GetName(), *Origin,
			*S2SStats::RoleName(It->GetLocalRole()), *DescribeHandles(*It));
	}
	UE_LOG(LogS2S, Display, TEXT("[S2S %s] inventory: projectiles%s"), *S2SStats::Who(), Line.IsEmpty() ? TEXT(" none") : *Line);
	int32 Ascs = 0;
	for (TActorIterator<AS2SAscActor> It(World); It; ++It)
	{
		Ascs++;
		UE_LOG(LogS2S, Display, TEXT("[S2S %s] inventory: ASC actor %s"), *S2SStats::Who(), *It->Describe());
	}
	if (Ascs == 0) UE_LOG(LogS2S, Display, TEXT("[S2S %s] inventory: no ASC actor"), *S2SStats::Who());
	if (PeerLink)
	{
		UE_LOG(LogS2S, Display, TEXT("[S2S %s] peer link: joined=%d, connection %s"), *S2SStats::Who(), PeerLink->IsJoined() ? 1 : 0,
			S2SDriver && S2SDriver->ServerConnection ? *S2SDriver->ServerConnection->Describe() : TEXT("none"));
	}
	if (BorderHits > 0) UE_LOG(LogS2S, Display, TEXT("[S2S %s] border hits applied on B so far: %d"), *S2SStats::Who(), BorderHits);
}

void US2SSpikeSubsystem::LogCues()
{
	for (const TPair<FString, int32>& Route : CueRoutes)
	{
		UE_LOG(LogS2S, Display, TEXT("[S2S %s] cue routes: %s x%d"), *S2SStats::Who(), *Route.Key, Route.Value);
	}
}

void US2SSpikeSubsystem::Finish()
{
	bFinished = true;
	UE_LOG(LogS2S, Display, TEXT("[S2S %s] ===== FINAL after %.1f s ====="), *S2SStats::Who(), FPlatformTime::Seconds() - StartedAt);
	LogTraffic(true);
	S2SStats::LogLatency(TEXT("CUMULATIVE"), true);
	LogInventory();
	LogCues();
	for (const FString& Result : GasResults) UE_LOG(LogS2S, Display, TEXT("[S2S %s] GAS RESULT %s"), *S2SStats::Who(), *Result);
	UE_LOG(LogS2S, Display, TEXT("[S2S %s] ===== END ====="), *S2SStats::Who());
	FPlatformMisc::RequestExit(false, TEXT("S2SSpike"));
}
