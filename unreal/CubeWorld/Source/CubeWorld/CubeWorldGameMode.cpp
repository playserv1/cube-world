#include "CubeWorldGameMode.h"
#include "CubeWorld.h"
#include "CubePlayerPawn.h"
#include "CubeHUD.h"
#include "CubeEntities.h"
#include "CubeBombs.h"
#include "CubeLiveTables.h"
#include "CubeWebSocketServer.h"
#include "PlayServ.h"
#include "Core/PlayServSettings.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "GameFramework/GameSession.h"
#include "GameFramework/PlayerController.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Guid.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
#include "TimerManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	constexpr int32 ChunkSize = 400;
	constexpr int64 PresenceTtlMs = 5000, RegionTtlMs = 30000, TombstoneTtlMs = 60000;

	FString ServerNameOf()
	{
		FString Name;
		if (FParse::Value(FCommandLine::Get(), TEXT("-servername="), Name) && !Name.IsEmpty()) return Name.ToLower();
		const FString Machine = FPlatformMisc::GetEnvironmentVariable(TEXT("PLAYSERV_MACHINE_ID"));
		return Machine.IsEmpty() ? FString(TEXT("local")) : Machine.Right(5).ToLower();
	}

	const TCHAR* RegionColorName(int32 Region) { return CubeSpec::RegionColorName(Region); }

	// The Unreal servers take the lower row of the die first (regions 3, 4, 5), the C# servers the upper; either
	// takes any free region once its own row is full.
	int32 RegionToTry(int32 Try) { return (Try + CubeSpec::RegionColumns) % CubeSpec::RegionCount; }
}

ACubeWorldGameMode::ACubeWorldGameMode()
{
	DefaultPawnClass = ACubePlayerPawn::StaticClass();
	HUDClass = ACubeHUD::StaticClass();
	PrimaryActorTick.bCanEverTick = true;
	bUseSeamlessTravel = false;
}

int64 ACubeWorldGameMode::Now()
{
	return (int64)(FDateTime::UtcNow() - FDateTime(1970, 1, 1)).GetTotalMilliseconds();
}

void ACubeWorldGameMode::ServerLog(const FString& Text) const
{
	UE_LOG(LogCubeWorld, Log, TEXT("%s: %s"), *RoomName(), *Text);
}

FString ACubeWorldGameMode::Color() const { return RegionColorName(Region); }

FString ACubeWorldGameMode::RoomName() const { return FString::Printf(TEXT("%s-%s"), *Color(), *ServerName); }
FString ACubeWorldGameMode::RoomSlug() const { return UPlayServSettings::GetRoomDefaultSlug(); }

// ── startup ──────────────────────────────────────────────────────────────────────────────────────

void ACubeWorldGameMode::BeginPlay()
{
	Super::BeginPlay();
	bDedicated = GetNetMode() == NM_DedicatedServer;
	if (!bDedicated) return;   // the menu map of a client: nothing to serve
	bOffline = CubeIsOffline();
	ServerName = bOffline ? FString(TEXT("offline")) : ServerNameOf();
	State = GetWorld()->SpawnActor<ACubeWorldState>();
	if (bOffline) { StartOffline(); return; }
	StartServer();
}

// -cubeoffline: no sign-in, no tables, no room. The region comes from the command line and the neighbours from -peers,
// so servers and crossings can be tried on one machine without touching the shared, live dev environment.
void ACubeWorldGameMode::StartOffline()
{
	int32 Wanted = -1;
	FParse::Value(FCommandLine::Get(), TEXT("-region="), Wanted);
	Region = FMath::Clamp(Wanted < 0 ? CubeSpec::RegionColumns : Wanted, 0, CubeSpec::RegionCount - 1);
	Regions.Empty();
	auto Add = [this](int32 R)
	{
		FCubeRegionRep Rep;
		Rep.Region = R; Rep.Room = CubeOfflineRoomName(R); Rep.Color = CubeSpec::RegionColorName(R); Rep.Server = ServerName; Rep.Slug = TEXT("cubeworld-ue");
		Regions.Add(Rep);
	};
	for (const FCubeOfflinePeer& Peer : CubeOfflinePeers()) Add(Peer.Region);
	if (!Regions.ContainsByPredicate([this](const FCubeRegionRep& R) { return R.Region == Region; })) Add(Region);
	State->Regions = Regions;
	ServerLog(FString::Printf(TEXT("offline: region %d, %d region(s) known, no platform"), Region, Regions.Num()));
	// The JSON door too, as on the platform: a client given this server as ws://host:port reaches it the way it reaches a
	// C# server, so crossings between the two kinds of connection can be tried here as well.
	OpenWebSocket();
	Serve();
}

void ACubeWorldGameMode::StartServer()
{
	TWeakObjectPtr<ACubeWorldGameMode> Weak(this);
	// On the platform's pool the process is handed a deployment token and no server key; on a developer's machine the
	// other way round. The SDK's parameterless LoginServer knows only the key, so the credential is picked here.
	const FString Token = UPlayServSettings::GetDeploymentToken();
	const FString Credential = Token.IsEmpty() ? UPlayServSettings::GetServerKey() : Token;
	PlayServ::Auth::LoginServer(Credential, FPlayServSimpleCallback::CreateLambda([Weak](bool bOk, const FPlayServError& Error)
	{
		if (!Weak.IsValid()) return;
		if (!bOk) { Weak->RetryStartup(FString::Printf(TEXT("server sign-in failed: %s"), *Error.Message)); return; }
		Weak->LoadWorld();
	}));
}

void ACubeWorldGameMode::RetryStartup(const FString& Why)
{
	if (bClosing) return;
	ServerLog(FString::Printf(TEXT("world not ready, retrying in 5 s: %s"), *Why));
	FTimerHandle H;
	GetWorldTimerManager().SetTimer(H, this, &ACubeWorldGameMode::StartServer, 5.f, false);
}

void ACubeWorldGameMode::LoadWorld()
{
	TWeakObjectPtr<ACubeWorldGameMode> Weak(this);
	LastCubeAt = Now();
	PlayServ::Data::LoadAll<UWorldCube>(FPlayServFilter::None(), [Weak](bool bOk, TArray<UWorldCube*> Rows, const FPlayServError& Error)
	{
		if (!Weak.IsValid()) return;
		if (!bOk) { Weak->RetryStartup(FString::Printf(TEXT("cubes not loaded: %s"), *Error.Message)); return; }
		for (UWorldCube* Row : Rows) Weak->World.Apply(FIntVector(Row->x, Row->y, Row->z), FName(*Row->kind), Row->placed_by, Row->placed_on, Row->at, Row);
		Weak->LoadBombs();
	});
}

void ACubeWorldGameMode::LoadBombs()
{
	TWeakObjectPtr<ACubeWorldGameMode> Weak(this);
	LastBombAt = Now();
	PlayServ::Data::LoadAll<UWorldBomb>(FPlayServFilter::None(), [Weak](bool bOk, TArray<UWorldBomb*> Rows, const FPlayServError& Error)
	{
		if (!Weak.IsValid()) return;
		// The bombs are an extra: a world whose bombs cannot be read still opens, without them.
		if (!bOk) Weak->ServerLog(FString::Printf(TEXT("bombs not loaded, the world opens without them: %s"), *Error.Message));
		Weak->ApplyBombTable(Rows);
		Weak->bBombsLoaded = bOk;
		Weak->RegionTry = 0;
		Weak->ClaimRegion(0);
	});
}

// A server claims a free region, as the C# server does: one nobody holds, or whose holder has not been seen for 30 s.
void ACubeWorldGameMode::ClaimRegion(int32 Try)
{
	if (Try >= CubeSpec::RegionCount) { RetryStartup(TEXT("every region is held")); return; }
	const int32 Candidate = RegionToTry(Try);
	TWeakObjectPtr<ACubeWorldGameMode> Weak(this);
	PlayServ::Data::LoadAll<UWorldRegion>(FPlayServFilter::Where(TEXT("region")).EqualTo(FString::FromInt(Candidate)), [Weak, Try, Candidate](bool bOk, TArray<UWorldRegion*> Rows, const FPlayServError& Error)
	{
		if (!Weak.IsValid()) return;
		ACubeWorldGameMode* Self = Weak.Get();
		if (!bOk) { Self->RetryStartup(FString::Printf(TEXT("regions not read: %s"), *Error.Message)); return; }
		UWorldRegion* Holder = Rows.Num() > 0 ? Rows[0] : nullptr;
		if (Holder && Holder->server != Self->ServerName && Now() - Holder->seen_at < RegionTtlMs) { Self->ClaimRegion(Try + 1); return; }
		Self->Region = Candidate;
		Self->RegionRow.Reset(Holder ? Holder : PlayServ::Data::Create<UWorldRegion>());
		Self->WriteRegionClaim([Weak, Try, Candidate](bool bWritten)
		{
			if (!Weak.IsValid()) return;
			if (!bWritten) { Weak->Region = -1; Weak->RegionRow.Reset(); Weak->ClaimRegion(Try + 1); return; }
			// A second later the claim must still be ours: two servers may have reached for the same region.
			FTimerHandle H;
			Weak->GetWorldTimerManager().SetTimer(H, [Weak, Try, Candidate]()
			{
				if (!Weak.IsValid()) return;
				PlayServ::Data::LoadAll<UWorldRegion>(FPlayServFilter::Where(TEXT("region")).EqualTo(FString::FromInt(Candidate)), [Weak, Try, Candidate](bool bOk, TArray<UWorldRegion*> Rows, const FPlayServError&)
				{
					if (!Weak.IsValid()) return;
					if (bOk && Rows.Num() > 0 && Rows[0]->server == Weak->ServerName) { Weak->RegionRow.Reset(Rows[0]); Weak->OpenRoom(); return; }
					Weak->Region = -1; Weak->RegionRow.Reset();
					Weak->ClaimRegion(Try + 1);
				});
			}, 1.f, false);
		});
	});
}

void ACubeWorldGameMode::WriteRegionClaim(const TFunction<void(bool)>& Done)
{
	UWorldRegion* Row = RegionRow.Get();
	if (!Row || Region < 0) { Done(false); return; }
	Row->region = FString::FromInt(Region);
	Row->server = ServerName;
	Row->color = Color();
	Row->room = RoomName();
	Row->slug = RoomSlug();
	Row->seen_at = Now();
	TWeakObjectPtr<ACubeWorldGameMode> Weak(this);
	PlayServ::Data::Save(Row, FPlayServSimpleCallback::CreateLambda([Weak, Done](bool bOk, const FPlayServError& Error)
	{
		if (!Weak.IsValid()) return;
		if (!bOk) Weak->ServerLog(FString::Printf(TEXT("region claim not written: %s"), *Error.Message));
		Done(bOk);
	}));
}

// The public address players connect to: -PublicHost=, else the address PlayServ hosting hands a pool machine, else
// this machine's first adapter (accepted only on an environment set up for local development).
FString ACubeWorldGameMode::ResolveHost() const
{
	FString Host;
	if (FParse::Value(FCommandLine::Get(), TEXT("-PublicHost="), Host) && !Host.IsEmpty()) return Host;
	// A pool machine has a name under the platform's domain (its TLS certificate is for that name) and an address.
	Host = FPlatformMisc::GetEnvironmentVariable(TEXT("PLAYSERV_PUBLIC_HOST"));
	if (!Host.IsEmpty()) return Host;
	Host = FPlatformMisc::GetEnvironmentVariable(TEXT("PLAYSERV_PUBLIC_IP"));
	if (!Host.IsEmpty()) return Host;
	bool bCanBindAll = false;
	ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	return Sockets ? Sockets->GetLocalHostAddr(*GLog, bCanBindAll)->ToString(false) : FString(TEXT("127.0.0.1"));
}

void ACubeWorldGameMode::OpenRoom()
{
	ServerLog(FString::Printf(TEXT("%d changed blocks loaded, %d bombs, region %d claimed, opening the room"), World.Overrides.Num(), Bombs.Num(), Region));
	UPlayServRooms* Rooms = UPlayServSubsystem::Get() ? UPlayServSubsystem::Get()->GetRooms() : nullptr;
	if (Rooms)
	{
		Rooms->OnRoomEnded.RemoveDynamic(this, &ACubeWorldGameMode::HandleRoomEnded);
		Rooms->OnRoomEnded.AddDynamic(this, &ACubeWorldGameMode::HandleRoomEnded);
		Rooms->OnPlayerRemoved.RemoveDynamic(this, &ACubeWorldGameMode::HandlePlayerRemoved);
		Rooms->OnPlayerRemoved.AddDynamic(this, &ACubeWorldGameMode::HandlePlayerRemoved);
		Rooms->OnRoomPlacementChanged.RemoveDynamic(this, &ACubeWorldGameMode::HandleRoomPlacementChanged);
		Rooms->OnRoomPlacementChanged.AddDynamic(this, &ACubeWorldGameMode::HandleRoomPlacementChanged);
		Rooms->SetReconnectGraceSeconds(0.f);
	}
	TWeakObjectPtr<ACubeWorldGameMode> Weak(this);
	// A process PlayServ hosting started for one room registers that room; one we run ourselves opens its own.
	if (!PlayServ::Rooms::GetLaunchRoomName().IsEmpty())
	{
		PlayServ::Rooms::StartRoomPlayServHosted(FPlayServSimpleCallback::CreateLambda([Weak](bool bOk, const FPlayServError& Error)
		{
			if (!Weak.IsValid()) return;
			if (!bOk) { Weak->ServerLog(FString::Printf(TEXT("the launch room was refused: %s; exiting"), *Error.Message)); FPlatformMisc::RequestExit(false); return; }
			Weak->Serve();
		}));
		return;
	}
	PlayServ::Rooms::StartHosting(FPlayServSimpleCallback::CreateLambda([Weak](bool bOk, const FPlayServError& Error)
	{
		if (!Weak.IsValid()) return;
		ACubeWorldGameMode* Self = Weak.Get();
		if (!bOk) { Self->RetryStartup(FString::Printf(TEXT("hosting refused: %s (%s)"), *Error.Message, *Error.ProblemCode)); return; }
		Self->OpenWebSocket();
		FPlayServRoomSnapshot Room;
		Room.RoomName = Self->RoomName();
		Room.State = TEXT("open");
		Room.Attributes.Add(TEXT("engine"), TEXT("unreal"));
		if (Self->Web.IsValid()) Room.Attributes.Add(TEXT("ws"), Self->WebAddress());
		Room.Attributes.Add(PlayServ::Rooms::Attributes::Name, FString::Printf(TEXT("Cube World %s"), *Self->Color()));
		Room.Attributes.Add(TEXT("color"), Self->Color());
		Room.Attributes.Add(TEXT("region"), FString::FromInt(Self->Region));
		// What the platform told the process about its network, for reading off the room while the pool is new.
		for (const TCHAR* Var : { TEXT("PLAYSERV_PORTS_MAPPING"), TEXT("PLAYSERV_ROOM_LISTEN_PORT"), TEXT("PLAYSERV_PUBLIC_HOST"), TEXT("PLAYSERV_PUBLIC_IP") })
		{
			const FString Value = FPlatformMisc::GetEnvironmentVariable(Var);
			if (!Value.IsEmpty()) Room.Attributes.Add(FString(Var).ToLower(), Value);
		}
		// Iris plays over UDP on the game port. On a pool machine the platform's front offers wss on a public port and
		// forwards it to the door (PLAYSERV_PORTS_MAPPING), and the machine's certificate follows a room that registers
		// that front as its connect, as the C# servers do; the UDP address then rides in the attribute udp, which the
		// Unreal client reads. Elsewhere the connect is the UDP address itself.
		const FString Udp = FString::Printf(TEXT("%s:%d"), *Self->ResolveHost(), Self->GetWorld()->URL.Port);
		Room.Attributes.Add(TEXT("udp"), Udp);
		int32 FrontPort = 0;
		{
			const FString Mapping = FPlatformMisc::GetEnvironmentVariable(TEXT("PLAYSERV_PORTS_MAPPING"));
			TArray<TSharedPtr<FJsonValue>> Entries;
			if (!Mapping.IsEmpty() && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Mapping), Entries))
				for (const TSharedPtr<FJsonValue>& Entry : Entries)
				{
					const TSharedPtr<FJsonObject>* Object;
					if (Entry->TryGetObject(Object) && (*Object)->GetStringField(TEXT("protocol")) == TEXT("wss")) FrontPort = (int32)(*Object)->GetNumberField(TEXT("external_port"));
				}
		}
		if (FrontPort > 0)
		{
			Room.Connect.Host = FPlatformMisc::GetEnvironmentVariable(TEXT("PLAYSERV_PUBLIC_HOST"));
			Room.Connect.Port = FrontPort;
			Room.Connect.Transport = EPlayServRoomTransport::Wss;
		}
		else
		{
			Room.Connect.Host = Self->ResolveHost();
			Room.Connect.Port = Self->GetWorld()->URL.Port;
			Room.Connect.Transport = EPlayServRoomTransport::Udp;
		}
		Self->ServerLog(FString::Printf(TEXT("registering at %s:%d (%s), iris at %s"), *Room.Connect.Host, Room.Connect.Port, FrontPort > 0 ? TEXT("wss") : TEXT("udp"), *Udp));
		PlayServ::Rooms::StartRoom(Room, FPlayServSimpleCallback::CreateLambda([Weak](bool bRegistered, const FPlayServError& RegisterError)
		{
			if (!Weak.IsValid()) return;
			if (!bRegistered) { Weak->RetryStartup(FString::Printf(TEXT("room refused: %s (%s)"), *RegisterError.Message, *RegisterError.ProblemCode)); return; }
			Weak->Serve();
		}));
	}));
}

void ACubeWorldGameMode::Serve()
{
	bServing = true;
	State->Server = ServerName;
	State->Color = Color();
	State->Region = Region;
	ServerLog(TEXT("world ready"));
	if (bOffline) { GetWorldTimerManager().SetTimer(MoveTimer, this, &ACubeWorldGameMode::ShareMoves, 0.1f, true); return; }
	// Every write and delete of a block comes over the uplink; the other servers' writes also arrive over the live
	// tables, and the polls behind them are the fallback while that socket is down.
	SubscribeUplinkCubes();
	OpenLiveTables();
	GetWorldTimerManager().SetTimer(MoveTimer, this, &ACubeWorldGameMode::ShareMoves, 0.1f, true);
	GetWorldTimerManager().SetTimer(CubeTimer, this, &ACubeWorldGameMode::PollCubes, 5.f, true, 1.2f);
	GetWorldTimerManager().SetTimer(PresenceTimer, this, &ACubeWorldGameMode::PollPresence, 2.f, true, 1.1f);
	GetWorldTimerManager().SetTimer(HitTimer, this, &ACubeWorldGameMode::PollHits, 5.f, true, 1.3f);
	GetWorldTimerManager().SetTimer(BombTimer, this, &ACubeWorldGameMode::PollBombs, 5.f, true, 1.4f);
	GetWorldTimerManager().SetTimer(RegionTimer, this, &ACubeWorldGameMode::Heartbeat, 5.f, true, 0.f);
}

void ACubeWorldGameMode::EndPlay(const EEndPlayReason::Type Reason)
{
	if (UPlayServRooms* Rooms = UPlayServSubsystem::Get() ? UPlayServSubsystem::Get()->GetRooms() : nullptr)
	{
		Rooms->OnDataUpdate.RemoveAll(this);
		Rooms->OnDataSubscribed.RemoveAll(this);
	}
	if (LiveTables.IsValid()) { LiveTables->Shutdown(); LiveTables.Reset(); }
	if (Web.IsValid()) { Web->Shutdown(); Web.Reset(); }
	if (bDedicated && Reason != EEndPlayReason::LevelTransition) PlayServ::Rooms::StopHosting();
	Super::EndPlay(Reason);
}

// ── logins ───────────────────────────────────────────────────────────────────────────────────────

void ACubeWorldGameMode::PreLogin(const FString& Options, const FString& Address, const FUniqueNetIdRepl& UniqueId, FString& ErrorMessage)
{
	Super::PreLogin(Options, Address, UniqueId, ErrorMessage);
	if (!ErrorMessage.IsEmpty() || !bDedicated) return;
	if (!bServing || bClosing) { ErrorMessage = TEXT("server_not_ready"); return; }
	if (bOffline) return;   // nobody hands out tickets offline
	const FPlayServTicketVerdict Verdict = PlayServ::Rooms::VerifyTicket(PlayServ::Rooms::TicketFromOptions(Options));
	if (!Verdict.bAccepted) ErrorMessage = Verdict.ErrorMessage;
}

void ACubeWorldGameMode::PostLogin(APlayerController* NewPlayer)
{
	Super::PostLogin(NewPlayer);
	if (!bDedicated || NewPlayer->IsLocalController()) return;
	if (ACubePlayerPawn* Pawn = Cast<ACubePlayerPawn>(NewPlayer->GetPawn()))
	{
		Pawn->PlayerId = PlayServ::Rooms::GetPlayerId(NewPlayer);
		// Offline the client names itself, the same on every server of the run, so a crossing keeps who they are.
		if (bOffline) Pawn->PlayerId = OfflineIds.FindRef(NewPlayer);
		// A development build admits a player without a ticket; they get a local identity, unknown to the other servers.
		if (Pawn->PlayerId.IsEmpty()) Pawn->PlayerId = FString::Printf(TEXT("local-%s-%d"), *ServerName, ++LocalIds);
	}
}

FString ACubeWorldGameMode::InitNewPlayer(APlayerController* NewPlayerController, const FUniqueNetIdRepl& UniqueId, const FString& Options, const FString& Portal)
{
	if (bOffline && NewPlayerController) OfflineIds.Add(NewPlayerController, UGameplayStatics::ParseOption(Options, TEXT("cubeplayer")));
	return Super::InitNewPlayer(NewPlayerController, UniqueId, Options, Portal);
}

void ACubeWorldGameMode::Logout(AController* Exiting)
{
	// The pawn is already unpossessed here: the player is found by their controller.
	if (bDedicated && Exiting && !Exiting->IsLocalController())
		if (FCubeServerPlayer* Player = PlayerOfController(Exiting)) RemovePlayer(Player->Id);
	Super::Logout(Exiting);
}

FCubeServerPlayer* ACubeWorldGameMode::PlayerOf(ACubePlayerPawn* Pawn)
{
	if (!Pawn) return nullptr;
	for (auto& Pair : Players) if (Pair.Value->Pawn.Get() == Pawn) return Pair.Value.Get();
	return nullptr;
}

FCubeServerPlayer* ACubeWorldGameMode::PlayerOfController(AController* Controller)
{
	if (!Controller) return nullptr;
	for (auto& Pair : Players) if (Pair.Value->Controller.Get() == Controller) return Pair.Value.Get();
	return nullptr;
}

FCubeServerPlayer* ACubeWorldGameMode::PlayerById(const FString& Id)
{
	const TSharedPtr<FCubeServerPlayer>* Found = Players.Find(Id);
	return Found ? Found->Get() : nullptr;
}

void ACubeWorldGameMode::Spawn(FCubeServerPlayer& P)
{
	const FVector2D Centre = CubeSpec::RegionCentre(Region);
	P.X = Centre.X; P.Y = Centre.Y; P.Z = 0;
	P.Health = CubeSpec::MaxHealth;
}

// A player another server saw in the last 5 s, alive, goes on with the health that server last gave them. Seen in this
// region or near its border, they walked over it: they stand where that server last saw them, moved inside the region,
// unless their hello says where they crossed (newer). Seen farther away, they jumped here from the server list, and
// start at the region's spawn: a jump moves them, it does not heal them. The crossing takes the hand's charge with it,
// so a player cannot cross for a full-strength hit. Anyone else starts at the spawn, whole. World.Arriving on the C# side.
void ACubeWorldGameMode::Arrive(FCubeServerPlayer& P, const FVector* HelloPos)
{
	Spawn(P);
	const FCubeElsewhere* Seen = Elsewhere.Find(P.Id);
	const bool bHeard = Seen && CubeSeenJustNow(Seen->SeenAt, Seen->Pose.Health, Now());
	if (bHeard)
	{
		P.Health = Seen->Pose.Health;
		double X = Seen->Pose.X, Y = Seen->Pose.Y;
		if (CubeCrossedInto(Region, X, Y))
		{
			P.X = X; P.Y = Y; P.Z = Seen->Pose.Z; P.Yaw = Seen->Pose.Yaw; P.Pitch = Seen->Pose.Pitch;
			P.bSneaking = Seen->Pose.bSneaking; P.bSprinting = Seen->Pose.bSprinting;
		}
	}
	if (HelloPos)
	{
		P.X = FMath::Clamp(HelloPos->X, 0.0, (double)CubeSpec::Width_);
		P.Y = FMath::Clamp(HelloPos->Y, 0.0, (double)CubeSpec::Depth);
		P.Z = FMath::Clamp(HelloPos->Z, (double)CubeSpec::MinZ, CubeSpec::MaxZ + 8.0);
	}
	if (bHeard || HelloPos) P.LastAttackTick = TickCount;
	P.Moves.Reset(P.X, P.Y, P.Z, Now());
}

void ACubeWorldGameMode::OnHello(ACubePlayerPawn* Pawn, const FString& Name, bool bCross, double X, double Y, double Z)
{
	if (!bServing || !Pawn || PlayerOf(Pawn)) return;
	APlayerController* PC = Cast<APlayerController>(Pawn->GetController());
	if (Pawn->PlayerId.IsEmpty() && PC) Pawn->PlayerId = PlayServ::Rooms::GetPlayerId(PC);
	if (Pawn->PlayerId.IsEmpty()) Pawn->PlayerId = FString::Printf(TEXT("local-%s-%d"), *ServerName, ++LocalIds);
	if (FCubeServerPlayer* Twice = PlayerById(Pawn->PlayerId)) Players.Remove(Twice->Id);

	TSharedPtr<FCubeServerPlayer> Player = MakeShared<FCubeServerPlayer>();
	Player->Pawn = Pawn;
	Player->Controller = Pawn->GetController();
	Player->Id = Pawn->PlayerId;
	Player->Name = Name.IsEmpty() ? Pawn->PlayerId : Name.Left(32);
	const FVector Crossed(X, Y, Z);
	Arrive(*Player, bCross ? &Crossed : nullptr);
	Players.Add(Player->Id, Player);
	ServerLog(FString::Printf(TEXT("%s %s"), *Player->Name, bCross ? TEXT("crossed in") : TEXT("joined")));
	LoadInventoryAndWelcome(Player->Id);
}

// The inventory is theirs from wherever they last played; a first-timer gets a full stack of everything. The row is
// only read: writing it straight back raced the old server's last write after a crossing and could undo it. A row
// heard while the read is out is newer than what the read returns, and is taken instead.
void ACubeWorldGameMode::LoadInventoryAndWelcome(const FString& Id, int32 Attempt)
{
	if (bOffline)
	{
		if (FCubeServerPlayer* P = PlayerById(Id)) { P->Inventory = FCubeInventory::Starting(); P->bInventoryRead = true; Welcome(*P); }
		return;
	}
	TWeakObjectPtr<ACubeWorldGameMode> Weak(this);
	PlayServ::Data::LoadAll<UCubeInventory>(FPlayServFilter::Where(TEXT("player_id")).EqualTo(Id), [Weak, Id, Attempt](bool bOk, TArray<UCubeInventory*> Rows, const FPlayServError& Error)
	{
		if (!Weak.IsValid()) return;
		ACubeWorldGameMode* Self = Weak.Get();
		FCubeServerPlayer* P = Self->PlayerById(Id);
		if (!P) return;
		if (!bOk && Attempt < 2)
		{
			Self->ServerLog(FString::Printf(TEXT("inventory of %s not read, again in 2 s: %s"), *P->Name, *Error.Message));
			FTimerHandle Again;
			Self->GetWorldTimerManager().SetTimer(Again, FTimerDelegate::CreateWeakLambda(Self, [Self, Id, Attempt]() { Self->LoadInventoryAndWelcome(Id, Attempt + 1); }), 2.f, false);
			return;
		}
		if (!bOk)
		{
			// The player plays with the starting stacks meanwhile, which are not written over the row they may have.
			Self->ServerLog(FString::Printf(TEXT("inventory of %s not read: %s; the starting stacks until it is"), *P->Name, *Error.Message));
			P->Inventory = FCubeInventory::Starting();
			P->InventorySync.Base = P->Inventory;
			P->bInventoryRead = true;
			Self->Welcome(*P);
			FTimerHandle Later;
			Self->GetWorldTimerManager().SetTimer(Later, FTimerDelegate::CreateWeakLambda(Self, [Self, Id]() { Self->ReadInventoryAgain(Id); }), 10.f, false);
			return;
		}
		const bool bNew = Rows.Num() == 0;
		if (bNew)
		{
			UCubeInventory* Row = PlayServ::Data::Create<UCubeInventory>();
			Row->player_id = Id;
			P->InventoryRow.Reset(Row);
			P->Inventory = FCubeInventory::Starting();
		}
		else
		{
			P->InventoryRow.Reset(Rows[0]);
			P->Inventory = FCubeInventory::Parse(Rows[0]->stacks);
		}
		P->InventorySync.Base = P->Inventory;
		P->bInventoryRead = true;
		if (P->InventoryHeardWhileReading.IsSet())
		{
			const FCubeInventory Heard = P->InventoryHeardWhileReading.GetValue();
			P->InventoryHeardWhileReading.Reset();
			Self->HearInventory(Id, Heard);
		}
		if (bNew) Self->WriteInventory(*P);
		Self->Welcome(*P);
	});
}

void ACubeWorldGameMode::ReadInventoryAgain(const FString& Id)
{
	TWeakObjectPtr<ACubeWorldGameMode> Weak(this);
	PlayServ::Data::LoadAll<UCubeInventory>(FPlayServFilter::Where(TEXT("player_id")).EqualTo(Id), [Weak, Id](bool bOk, TArray<UCubeInventory*> Rows, const FPlayServError& Error)
	{
		if (!Weak.IsValid()) return;
		ACubeWorldGameMode* Self = Weak.Get();
		FCubeServerPlayer* P = Self->PlayerById(Id);
		if (!P || P->InventoryRow.IsValid()) return;
		if (!bOk)
		{
			FTimerHandle Later;
			Self->GetWorldTimerManager().SetTimer(Later, FTimerDelegate::CreateWeakLambda(Self, [Self, Id]() { Self->ReadInventoryAgain(Id); }), 10.f, false);
			return;
		}
		// What the player did since they came, on top of the row as it is: the base they played from was the starting stacks.
		UCubeInventory* Row = Rows.Num() > 0 ? Rows[0] : PlayServ::Data::Create<UCubeInventory>();
		Row->player_id = Id;
		P->InventoryRow.Reset(Row);
		const FCubeInventory Theirs = Rows.Num() > 0 ? FCubeInventory::Parse(Row->stacks) : P->InventorySync.Base;
		FCubeInventory Merged;
		if (P->InventorySync.Heard(P->Inventory, Theirs, Self->Now(), Merged)) P->Inventory = Merged;
		Self->ServerLog(FString::Printf(TEXT("inventory of %s read at last"), *P->Name));
		Self->ShareInventory(*P);
	});
}

void ACubeWorldGameMode::HearInventory(const FString& PlayerId, const FCubeInventory& Theirs)
{
	FCubeServerPlayer* P = PlayerById(PlayerId);
	if (!P) return;
	if (!P->bInventoryRead) { P->InventoryHeardWhileReading = Theirs; return; }
	FCubeInventory Merged;
	if (!P->InventorySync.Heard(P->Inventory, Theirs, Now(), Merged)) return;
	const bool bChangedHere = !Merged.Same(P->Inventory);
	P->Inventory = Merged;
	// The row lacks what the player did here (another writer's row came after this server's): it is written again.
	if (!Merged.Same(Theirs)) WriteInventory(*P);
	if (bChangedHere) SendInventory(*P);
}

void ACubeWorldGameMode::Welcome(FCubeServerPlayer& P)
{
	if (P.WebClient) { WebWelcome(P); return; }
	ACubePlayerPawn* Pawn = P.Pawn.Get();
	if (!Pawn) return;
	P.bWelcomed = true;
	P.bMoved = true;
	TArray<FCubeCellRep> Cells;
	for (const auto& Pair : World.Overrides) Cells.Add({ (int16)Pair.Key.X, (int16)Pair.Key.Y, (int16)Pair.Key.Z, CubeSpec::KindIndex(Pair.Value.Kind) });
	const int32 Chunks = FMath::Max(1, FMath::DivideAndRoundUp(Cells.Num(), ChunkSize));

	FCubeWelcomeRep W;
	W.Server = ServerName; W.Color = Color(); W.Room = RoomName(); W.Region = Region;
	W.X = P.X; W.Y = P.Y; W.Z = P.Z; W.Health = P.Health;
	W.Chunks = Chunks;
	TArray<FCubeStackRep> Stacks;
	for (const auto& Pair : P.Inventory.Stacks) Stacks.Add({ CubeSpec::KindIndex(Pair.Key), Pair.Value });
	Pawn->ClientWelcome(W, Stacks);
	for (int32 I = 0; I < Chunks; I++)
	{
		TArray<FCubeCellRep> Chunk;
		for (int32 J = I * ChunkSize; J < FMath::Min(Cells.Num(), (I + 1) * ChunkSize); J++) Chunk.Add(Cells[J]);
		Pawn->ClientWorldChunk(Chunk, I == Chunks - 1);
	}
	TArray<FCubeBombRep> Frames;
	for (const auto& Pair : Bombs)
	{
		if (Pair.Value.Record.State == TEXT("held") && Pair.Value.Record.Holder == P.Id) P.Bomb = Pair.Key;
		Frames.Add(BombFrame(Pair.Value.Record, &Pair.Value));
	}
	Pawn->ClientBombs(Frames);
	PublishPlayers();
}

// ── the loops ────────────────────────────────────────────────────────────────────────────────────

void ACubeWorldGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (Web.IsValid()) Web->Tick();
	if (!bServing) return;
	Accumulator += FMath::Min(DeltaSeconds, 0.25f);
	while (Accumulator >= CubeSpec::TickSeconds) { GameTick(); Accumulator -= CubeSpec::TickSeconds; }
}

/** The 20 Hz game tick: digging progresses and finishes, health regenerates, bombs come down and fly. */
void ACubeWorldGameMode::GameTick()
{
	TickCount++;
	for (auto& Pair : Players)
	{
		FCubeServerPlayer& P = *Pair.Value;
		TickDig(P);
		if (!P.bDead && P.Health < CubeSpec::MaxHealth && TickCount % CubeSpec::RegenIntervalTicks == 0)
		{
			P.Health = FMath::Min(CubeSpec::MaxHealth, P.Health + 1);
			P.bMoved = true;
		}
	}
	TickBombs();
	// What the other servers changed this tick goes out together (a blast elsewhere is a hundred blocks); a reset, or a
	// read of the table that finds thousands, goes out in chunks the size of the welcome's.
	if (Heard.Num() == 0) return;
	for (int32 I = 0; I < Heard.Num(); I += ChunkSize)
		BroadcastCubes(TArray<FCubeChange>(Heard.GetData() + I, FMath::Min(ChunkSize, Heard.Num() - I)), TArray<FCubeFall>(), true);
	Heard.Empty();
}

void ACubeWorldGameMode::ShareMoves()
{
	static int64 Round = 0;
	Round++;
	if (Round % 2 == 0)
		for (auto& Pair : Players)
		{
			FCubeServerPlayer& P = *Pair.Value;
			if (P.bWelcomed && (P.bMoved || Now() - P.PresenceWrittenAt > 2000)) WritePresence(P);
		}
	PublishPlayers();
	WebBroadcastPlayers();
}

FCubePresenceRep ACubeWorldGameMode::PoseOf(const FCubeServerPlayer& P) const
{
	FCubePresenceRep R;
	R.Id = P.Id; R.Name = P.Name; R.Server = ServerName; R.Color = Color();
	R.X = P.X; R.Y = P.Y; R.Z = P.Z; R.Yaw = P.Yaw; R.Pitch = P.Pitch; R.Health = P.Health;
	R.bSneaking = P.bSneaking; R.bSprinting = P.bSprinting;
	return R;
}

void ACubeWorldGameMode::PublishPlayers()
{
	if (!State) return;
	TArray<FCubePresenceRep> Everyone;
	for (const auto& Pair : Players) if (Pair.Value->bWelcomed) Everyone.Add(PoseOf(*Pair.Value));
	const int64 T = Now();
	for (const auto& Pair : Elsewhere)
		if (T - Pair.Value.SeenAt < PresenceTtlMs && !Players.Contains(Pair.Key)) Everyone.Add(Pair.Value.Pose);
	State->Players = Everyone;
}

// The claim is written, then the regions read: a read in flight during the write would leave the SDK holding the
// row's older version, and the next write would be refused.
void ACubeWorldGameMode::Heartbeat()
{
	WebBroadcastRegions();
	TWeakObjectPtr<ACubeWorldGameMode> Weak(this);
	if (RegionRow.IsValid()) WriteRegionClaim([Weak](bool) { if (Weak.IsValid()) Weak->PollRegions(); });
	else PollRegions();
}

// ── digging ──────────────────────────────────────────────────────────────────────────────────────

void ACubeWorldGameMode::TickDig(FCubeServerPlayer& P)
{
	if (!P.Dig.IsSet()) return;
	FCubeDig& Dig = P.Dig.GetValue();
	const double Ex = P.X, Ey = P.Y, Ez = P.Z + EyeHeightOf(P.bSneaking);
	if (FCubeServerWorld::DistanceToBlock(Ex, Ey, Ez, Dig.X, Dig.Y, Dig.Z) > CubeSpec::BlockReach + CubeSpec::ReachTolerance || P.bDead || !InThisRegion(P))
	{
		StopDig(P);
		return;
	}
	const int64 Elapsed = TickCount - Dig.StartTick;
	const int32 Stage = (int32)FMath::Min<int64>(9, Elapsed * 10 / Dig.Ticks);
	if (Stage != Dig.Stage)
	{
		Dig.Stage = Stage;
		BroadcastDig(P.Id, Dig.X, Dig.Y, Dig.Z, Stage);
	}
	if (Elapsed < Dig.Ticks) return;

	const int32 X = Dig.X, Y = Dig.Y, Z = Dig.Z;
	P.Dig.Reset();
	BroadcastDig(P.Id, X, Y, Z, -1);
	FCubeWorldUpdate Update;
	FBlockDef Broken;
	if (!World.Break(X, Y, Z, P.Id, ServerName, Update, Broken)) return;
	if (Broken.Drop != NAME_None && P.Inventory.Give(Broken.Drop)) ShareInventory(P);
	Publish(Update);
}

void ACubeWorldGameMode::StopDig(FCubeServerPlayer& P)
{
	if (!P.Dig.IsSet()) return;
	const FCubeDig Dig = P.Dig.GetValue();
	P.Dig.Reset();
	if (State) BroadcastDig(P.Id, Dig.X, Dig.Y, Dig.Z, -1);
}

// ── what the players ask ─────────────────────────────────────────────────────────────────────────

void ACubeWorldGameMode::OnMove(FCubeServerPlayer* P, double X, double Y, double Z, double Yaw, double Pitch, bool bOnGround, bool bSneaking, bool bSprinting, TOptional<double> SaidPeak, TOptional<int32> SaidSeq)
{
	if (!P || P->bDead) return;
	X = FMath::Clamp(X, 0.0, (double)CubeSpec::Width_);
	Y = FMath::Clamp(Y, 0.0, (double)CubeSpec::Depth);
	Z = FMath::Clamp(Z, (double)CubeSpec::MinZ, CubeSpec::MaxZ + 8.0);
	switch (P->Moves.Check(X, Y, Z, SaidSeq, Now()))
	{
	case ECubeMoveVerdict::Stale: return;
	case ECubeMoveVerdict::Refused: Correct(*P, X, Y, Z); return;
	default: break;
	}
	P->X = X; P->Y = Y; P->Z = Z;
	P->Yaw = Yaw; P->Pitch = Pitch;
	P->bSneaking = bSneaking; P->bSprinting = bSprinting;
	P->bMoved = true;

	// Fall damage, from the height reached since the player last stood on the ground (on this server or, over a border,
	// the one before: the client says its own peak).
	const double Damage = P->Fall.Step(P->Z, bOnGround, SaidPeak);
	if (Damage > 0) Hurt(*P, Damage, false, 0, 0, 0, FString());
}

void ACubeWorldGameMode::OnDig(FCubeServerPlayer* P, int32 X, int32 Y, int32 Z, bool bStart)
{
	if (!P) return;
	StopDig(*P);
	if (!bStart || P->bDead) return;
	const FBlockDef& Block = World.BlockAt(X, Y, Z);
	const double Ex = P->X, Ey = P->Y, Ez = P->Z + EyeHeightOf(P->bSneaking);
	if (!FCubeServerWorld::Inside(X, Y, Z) || !Block.IsSolid() || !Block.IsBreakable() || !InThisRegion(*P)
		|| FCubeServerWorld::DistanceToBlock(Ex, Ey, Ez, X, Y, Z) > CubeSpec::BlockReach + CubeSpec::ReachTolerance) return;
	P->Dig = FCubeDig{ X, Y, Z, TickCount, Block.BreakTicks, 0 };
	BroadcastDig(P->Id, X, Y, Z, 0);
}

void ACubeWorldGameMode::OnPlace(FCubeServerPlayer* P, int32 X, int32 Y, int32 Z, int32 NX, int32 NY, int32 NZ, FName Kind)
{
	if (!P || P->bDead) return;
	const double Ex = P->X, Ey = P->Y, Ez = P->Z + EyeHeightOf(P->bSneaking);
	const bool bReachable = FCubeServerWorld::DistanceToBlock(Ex, Ey, Ez, X, Y, Z) <= CubeSpec::BlockReach + CubeSpec::ReachTolerance && InThisRegion(*P);
	FCubeWorldUpdate Update;
	const bool bPlaced = bReachable && P->Inventory.Count(Kind) > 0 && World.Place(X, Y, Z, NX, NY, NZ, Kind, P->Id, ServerName, Hitboxes(), Update);
	if (!bPlaced) { SendInventory(*P, true); return; }
	P->Inventory.Take(Kind);
	ShareInventory(*P);
	Publish(Update);
}

// A server digs and places only for the players in its region: one standing anywhere else edits through that region's
// server. The C# servers' InThisRegion.
bool ACubeWorldGameMode::InThisRegion(const FCubeServerPlayer& P) const
{
	if (CubeNear(Region, P.X, P.Y, CubeSpec::BorderSlack)) return true;
	const int32 There = CubeSpec::RegionOf(P.X, P.Y);
	return !Regions.ContainsByPredicate([&](const FCubeRegionRep& R) { return R.Region == There && R.Server != ServerName; });
}

/** A hit on whoever is within reach: a player on this server, or one another server hosts. */
void ACubeWorldGameMode::OnAttack(FCubeServerPlayer* P, const FString& Target)
{
	if (!P || P->bDead || Target.IsEmpty() || Target == P->Id) return;
	FCubeServerPlayer* Local = PlayerById(Target);
	const FCubeElsewhere* Remote = Local ? nullptr : Elsewhere.Find(Target);
	if (!Local && !Remote) return;
	if (Local && (Local->bDead || Local->Health <= 0)) return;
	if (Remote && (Remote->Pose.Health <= 0 || Now() - Remote->SeenAt > PresenceTtlMs)) return;
	const FCubePresenceRep Pose = Local ? PoseOf(*Local) : Remote->Pose;

	// A hand recharges in 5 ticks; a hit before that is weaker: 20 % plus 80 % of the charge squared.
	const double Charge = FMath::Min(1.0, (TickCount - P->LastAttackTick) / (double)CubeSpec::FistChargeTicks);
	P->LastAttackTick = TickCount;

	const double Ex = P->X, Ey = P->Y, Ez = P->Z + EyeHeightOf(P->bSneaking);
	if (FCubeServerWorld::DistanceToHitbox(Ex, Ey, Ez, HitboxOf(Pose)) > CubeSpec::EntityReach + CubeSpec::ReachTolerance) return;

	const double Damage = CubeSpec::FistDamage * (0.2 + 0.8 * Charge * Charge);
	double Dx = Pose.X - P->X, Dy = Pose.Y - P->Y;
	double Length = FMath::Sqrt(Dx * Dx + Dy * Dy);
	if (Length < 1e-4) { Dx = -FMath::Sin(P->Yaw); Dy = FMath::Cos(P->Yaw); Length = 1; }
	const double Strength = CubeSpec::Knockback + (P->bSprinting ? CubeSpec::SprintKnockback : 0);

	if (Local) { Hurt(*Local, Damage, true, Dx / Length, Dy / Length, Strength, P->Id); return; }
	// The victim is on another server: hand the hit over through platform data; that server applies it.
	WriteHit(FString::Printf(TEXT("%s:%lld:%s"), *P->Id, TickCount, *FGuid::NewGuid().ToString(EGuidFormats::Digits)), Target, P->Id, Damage, Dx / Length, Dy / Length, Strength);
}

void ACubeWorldGameMode::Hurt(FCubeServerPlayer& Victim, double Damage, bool bDirected, double DX, double DY, double Strength, const FString& By)
{
	if (Victim.bDead || TickCount - Victim.LastHurtTick < CubeSpec::InvulnerabilityTicks) return;
	Victim.LastHurtTick = TickCount;
	if (bDirected) Victim.Moves.Knocked(Strength);
	Victim.Health = FMath::Max(0.0, Victim.Health - Damage);
	Victim.bMoved = true;
	BroadcastHurt(Victim.Id, Victim.Health, bDirected ? DX * Strength : 0, bDirected ? DY * Strength : 0, bDirected ? Strength : 0, By);
	if (Victim.Health > 0) return;
	Victim.bDead = true;
	StopDig(Victim);
	BroadcastDeath(Victim.Id, By);
}

void ACubeWorldGameMode::OnRespawn(FCubeServerPlayer* P)
{
	if (!P || !P->bDead) return;
	Spawn(*P);
	P->bDead = false;
	P->Fall = FCubePlayerFall();
	P->Moves.Reset(P->X, P->Y, P->Z, Now());
	P->bMoved = true;
	SendRespawn(*P);
}

void ACubeWorldGameMode::Publish(const FCubeWorldUpdate& Update)
{
	for (const FCubeChange& C : Update.Changes) WriteCube(C.At);
	BroadcastCubes(Update.Changes, Update.Falls, false);
}

void ACubeWorldGameMode::BroadcastCubes(const TArray<FCubeChange>& Changes, const TArray<FCubeFall>& Falls, bool bRemote)
{
	if (!State || (Changes.Num() == 0 && Falls.Num() == 0)) return;
	TArray<FCubeChangeRep> ChangeReps;
	for (const FCubeChange& C : Changes) ChangeReps.Add({ (int16)C.At.X, (int16)C.At.Y, (int16)C.At.Z, CubeSpec::KindIndex(C.Kind), C.On });
	TArray<FCubeFallRep> FallReps;
	for (const FCubeFall& F : Falls) FallReps.Add({ CubeSpec::KindIndex(F.Kind), (int16)F.X, (int16)F.Y, (int16)F.FromZ, (int16)F.ToZ });
	State->MulticastCubes(ChangeReps, FallReps, bRemote);
	WebBroadcastCubes(Changes, Falls, bRemote);
}

void ACubeWorldGameMode::ShareInventory(FCubeServerPlayer& P)
{
	WriteInventory(P);
	SendInventory(P);
}

TArray<FCubeHitbox> ACubeWorldGameMode::Hitboxes() const
{
	TArray<FCubeHitbox> Out;
	for (const auto& Pair : Players) if (!Pair.Value->bDead) Out.Add(HitboxOf(*Pair.Value));
	const int64 T = Now();
	for (const auto& Pair : Elsewhere)
		if (T - Pair.Value.SeenAt < PresenceTtlMs && Pair.Value.Pose.Health > 0 && !Players.Contains(Pair.Key)) Out.Add(HitboxOf(Pair.Value.Pose));
	return Out;
}

TArray<TPair<FString, FCubeHitbox>> ACubeWorldGameMode::Targets() const
{
	TArray<TPair<FString, FCubeHitbox>> Out;
	for (const auto& Pair : Players) if (!Pair.Value->bDead) Out.Add(TPair<FString, FCubeHitbox>(Pair.Key, HitboxOf(*Pair.Value)));
	const int64 T = Now();
	for (const auto& Pair : Elsewhere)
		if (T - Pair.Value.SeenAt < PresenceTtlMs && Pair.Value.Pose.Health > 0 && !Players.Contains(Pair.Key)) Out.Add(TPair<FString, FCubeHitbox>(Pair.Key, HitboxOf(Pair.Value.Pose)));
	return Out;
}

FCubeHitbox ACubeWorldGameMode::HitboxOf(const FCubePresenceRep& Pose)
{
	return { Pose.X, Pose.Y, Pose.Z, Pose.bSneaking ? CubeSpec::SneakHeight : CubeSpec::Height };
}

FCubeHitbox ACubeWorldGameMode::HitboxOf(const FCubeServerPlayer& P)
{
	return { P.X, P.Y, P.Z, P.bSneaking ? CubeSpec::SneakHeight : CubeSpec::Height };
}

// ── bombs ────────────────────────────────────────────────────────────────────────────────────────
// Every server brings every free bomb down, but only the server of the region a bomb is over lets a player pick
// it up, so two servers never hand out one bomb. A thrown bomb is flown by its thrower's server alone.

/**
 * The bomb table as it is now: at start-up, and again on every new uplink subscription, which hears nothing of what was
 * written before it was in place. A bomb can have several rows (the drop function's and the servers'): the one furthest
 * on is the bomb. One the table says is over goes up here too, if this server still has it in play (it missed the end);
 * otherwise it is remembered as over, as the C# servers do, and no later row of it brings it back.
 */
void ACubeWorldGameMode::ApplyBombTable(const TArray<UWorldBomb*>& Rows)
{
	TMap<FString, FCubeBombRecord> Furthest;
	for (const UWorldBomb* Row : Rows)
	{
		const FCubeBombRecord R = RecordOf(Row);
		const FCubeBombRecord* Known = Furthest.Find(R.Id);
		if (!Known || FCubeBombRecord::Rank(R.State) > FCubeBombRecord::Rank(Known->State) || (FCubeBombRecord::Rank(R.State) == FCubeBombRecord::Rank(Known->State) && R.At > Known->At)) Furthest.Add(R.Id, R);
	}
	for (const auto& Pair : Furthest)
	{
		LastBombAt = FMath::Max(LastBombAt, Pair.Value.At);
		if (Pair.Value.IsOver() && !Bombs.Contains(Pair.Key)) BombsOver.Add(Pair.Key, Pair.Value.At);
		else if (Pair.Value.IsOver() && Now() - Pair.Value.At > 5000)
		{
			// An end missed long ago: the blast was worked out by every server that heard it then, and is not again
			// here. The bomb just goes out of play, as one that fizzled.
			FCubeBombRecord Gone = Pair.Value;
			Gone.State = TEXT("fizzled");
			OnBomb(Gone, false);
		}
		else OnBomb(Pair.Value, false);
	}
}

void ACubeWorldGameMode::ReloadBombs()
{
	if (bBombsReloading || bOffline) return;
	bBombsReloading = true;
	TWeakObjectPtr<ACubeWorldGameMode> Weak(this);
	PlayServ::Data::LoadAll<UWorldBomb>(FPlayServFilter::None(), [Weak](bool bOk, TArray<UWorldBomb*> Rows, const FPlayServError& Error)
	{
		if (!Weak.IsValid()) return;
		Weak->bBombsReloading = false;
		if (!bOk) { Weak->ServerLog(FString::Printf(TEXT("bombs not read again: %s"), *Error.Message)); return; }
		Weak->ApplyBombTable(Rows);
	});
}

FCubeBombRecord ACubeWorldGameMode::RecordOf(const UWorldBomb* Row)
{
	FCubeBombRecord R;
	R.Id = Row->bomb_id; R.State = Row->state; R.Holder = Row->holder;
	R.X = Row->x; R.Y = Row->y; R.Z = Row->z; R.VX = Row->vx; R.VY = Row->vy; R.VZ = Row->vz;
	R.DroppedAt = Row->dropped_at; R.At = Row->at;
	return R;
}

FCubeBombRecord ACubeWorldGameMode::Next(const FCubeBombRecord& Bomb, const FString& InState, const FString& Holder, double X, double Y, double Z) const
{
	FCubeBombRecord R;
	R.Id = Bomb.Id; R.State = InState; R.Holder = Holder; R.X = X; R.Y = Y; R.Z = Z; R.DroppedAt = Bomb.DroppedAt; R.At = Now();
	return R;
}

FCubeBombRep ACubeWorldGameMode::BombFrame(const FCubeBombRecord& Bomb, const FCubeLiveBomb* Live) const
{
	FCubeBombRep F;
	F.Id = Bomb.Id; F.State = Bomb.State; F.Holder = Bomb.Holder;
	F.X = Bomb.X; F.Y = Bomb.Y; F.Z = Bomb.Z; F.VX = Bomb.VX; F.VY = Bomb.VY; F.VZ = Bomb.VZ;
	F.AgeMs = (int32)FMath::Clamp<int64>(Now() - Bomb.At, 0, INT32_MAX);
	F.bHasHeight = Live != nullptr;
	F.Height = Live ? Live->Z : Bomb.Z;
	return F;
}

void ACubeWorldGameMode::TickBombs()
{
	TArray<FString> Ids;
	Bombs.GetKeys(Ids);
	for (const FString& Id : Ids)
	{
		FCubeLiveBomb* Live = Bombs.Find(Id);
		if (!Live) continue;
		const FCubeBombRecord Bomb = Live->Record;
		if (Bomb.State == TEXT("free"))
		{
			Live->Z = CubeBombs::Descend(World.Voxels, Bomb.X, Bomb.Y, Live->Z);
			if (CubeSpec::RegionOf(Bomb.X, Bomb.Y) != Region) continue;
			for (auto& Pair : Players)
			{
				FCubeServerPlayer& P = *Pair.Value;
				const FCubeHitbox Box = HitboxOf(P);
				const double Reach = CubeSpec::Width / 2 + CubeSpec::PickupReach;
				const bool bNear = FMath::Abs(Bomb.X - Box.X) <= Reach && FMath::Abs(Bomb.Y - Box.Y) <= Reach
					&& Live->Z >= Box.Z - CubeSpec::PickupReachUp && Live->Z <= Box.Z + Box.Height + CubeSpec::PickupReachUp;
				if (P.bWelcomed && !P.bDead && P.Bomb.IsEmpty() && bNear)
				{
					ShareBomb(Next(Bomb, TEXT("held"), P.Id, Bomb.X, Bomb.Y, Live->Z), false);
					break;
				}
			}
		}
		else if (Bomb.State == TEXT("flying") && Live->bOwned)
		{
			ECubeFlight Flight = ECubeFlight::Flying;
			if (Live->Age++ >= CubeSpec::BombFlightTicks) Flight = ECubeFlight::Exploded;
			else
			{
				// One tick of a thrown bomb, as a snowball flies: it moves by its motion, then the motion is multiplied by
				// the drag and gravity pulls it down. It explodes at the last free point before a block or at the player
				// it hits (its thrower only after a few ticks); off the edge of the world it is gone.
				const double Length = FMath::Sqrt(Live->V[0] * Live->V[0] + Live->V[1] * Live->V[1] + Live->V[2] * Live->V[2]);
				const int32 Steps = FMath::Max(1, FMath::CeilToInt32(Length / 0.1));
				const TArray<TPair<FString, FCubeHitbox>> Hit = Targets();
				for (int32 I = 1; I <= Steps && Flight == ECubeFlight::Flying; I++)
				{
					const double X = Live->P[0] + Live->V[0] / Steps, Y = Live->P[1] + Live->V[1] / Steps, Z = Live->P[2] + Live->V[2] / Steps;
					if (X < 0 || X >= CubeSpec::Width_ || Y < 0 || Y >= CubeSpec::Depth || Z < CubeSpec::MinZ) { Flight = ECubeFlight::Gone; break; }
					if (World.IsSolid(FMath::FloorToInt32(X), FMath::FloorToInt32(Y), FMath::FloorToInt32(Z))) { Flight = ECubeFlight::Exploded; break; }
					Live->P[0] = X; Live->P[1] = Y; Live->P[2] = Z;
					for (const auto& T : Hit)
					{
						const FCubeHitbox& B = T.Value;
						const bool bInside = FMath::Abs(X - B.X) <= CubeSpec::Width / 2 && FMath::Abs(Y - B.Y) <= CubeSpec::Width / 2 && Z >= B.Z && Z <= B.Z + B.Height;
						if ((T.Key != Bomb.Holder || Live->Age >= CubeSpec::OwnerImmunityTicks) && bInside) { Flight = ECubeFlight::Exploded; break; }
					}
				}
				if (Flight == ECubeFlight::Flying)
				{
					for (int32 A = 0; A < 3; A++) Live->V[A] *= CubeSpec::ProjectileDrag;
					Live->V[2] -= CubeSpec::ProjectileGravity;
				}
			}
			if (Flight == ECubeFlight::Exploded) { const double At[3] = { Live->P[0], Live->P[1], Live->P[2] }; Explode(Bomb, At); }
			else if (Flight == ECubeFlight::Gone) ShareBomb(Next(Bomb, TEXT("fizzled"), Bomb.Holder, Live->P[0], Live->P[1], Live->P[2]), false);
		}
	}
}

void ACubeWorldGameMode::OnThrow(FCubeServerPlayer* P, double DX, double DY, double DZ)
{
	if (!P || P->bDead || P->Bomb.IsEmpty()) return;
	const FString Id = P->Bomb;
	P->Bomb.Empty();
	const FCubeLiveBomb* Live = Bombs.Find(Id);
	if (!Live || Live->Record.State != TEXT("held") || Live->Record.Holder != P->Id) return;

	double Length = FMath::Sqrt(DX * DX + DY * DY + DZ * DZ);
	if (Length < 1e-6)
	{
		DX = -FMath::Sin(P->Yaw) * FMath::Cos(P->Pitch); DY = FMath::Cos(P->Yaw) * FMath::Cos(P->Pitch); DZ = -FMath::Sin(P->Pitch); Length = 1;
	}
	FCubeBombRecord Thrown = Next(Live->Record, TEXT("flying"), P->Id, P->X, P->Y, P->Z + EyeHeightOf(P->bSneaking));
	Thrown.VX = DX / Length * CubeSpec::ThrowSpeed; Thrown.VY = DY / Length * CubeSpec::ThrowSpeed; Thrown.VZ = DZ / Length * CubeSpec::ThrowSpeed;
	ShareBomb(Thrown, true);
}

/**
 * Players here are hurt, players elsewhere get a WorldHit, both judged against the world as it stood before the
 * blast, as Minecraft does. Then the bomb is recorded as exploded, and OnBomb breaks this region's blocks; every
 * other server breaks its own when it hears the record. The centre is rounded to a thousandth so the record carries
 * exactly the point every server works the blast out from.
 */
void ACubeWorldGameMode::Explode(const FCubeBombRecord& Bomb, const double* At)
{
	const double Cx = FMath::RoundToDouble(At[0] * 1000) / 1000, Cy = FMath::RoundToDouble(At[1] * 1000) / 1000, Cz = FMath::RoundToDouble(At[2] * 1000) / 1000;
	for (auto& Pair : Players)
	{
		FCubeServerPlayer& P = *Pair.Value;
		double Damage, Nx, Ny, Impact;
		if (!P.bDead && World.Blast(Cx, Cy, Cz, CubeSpec::BombPower, HitboxOf(P), EyeHeightOf(P.bSneaking), Damage, Nx, Ny, Impact))
			Hurt(P, Damage, true, Nx, Ny, Impact, Bomb.Holder);
	}
	const int64 T = Now();
	for (const auto& Pair : Elsewhere)
	{
		const FCubePresenceRep& Pose = Pair.Value.Pose;
		if (T - Pair.Value.SeenAt >= PresenceTtlMs || Pose.Health <= 0 || Players.Contains(Pair.Key)) continue;
		double Damage, Nx, Ny, Impact;
		if (World.Blast(Cx, Cy, Cz, CubeSpec::BombPower, HitboxOf(Pose), EyeHeightOf(Pose.bSneaking), Damage, Nx, Ny, Impact))
			WriteHit(FString::Printf(TEXT("%s:%s"), *Bomb.Id, *Pose.Id), Pose.Id, Bomb.Holder, Damage, Nx, Ny, Impact);
	}
	ShareBomb(Next(Bomb, TEXT("exploded"), Bomb.Holder, Cx, Cy, Cz), false);
}

/** The seed a bomb's blast is worked out with, the same on every server: FNV-1a of its id. */
static int32 BlastSeed(const FString& BombId)
{
	uint32 Hash = 2166136261u;
	const FTCHARToUTF8 Utf8(*BombId);
	for (int32 I = 0; I < Utf8.Length(); I++) Hash = (Hash ^ (uint8)Utf8.Get()[I]) * 16777619u;
	return (int32)(Hash & 0x7fffffff);
}

/**
 * A bomb went off, here or on another server: this server breaks the blocks of its own region and no others. A
 * region whose server is not up when the bomb goes off keeps its blocks.
 */
void ACubeWorldGameMode::Crater(const FCubeBombRecord& Bomb)
{
	if (Region < 0) return;
	FCubeWorldUpdate Update;
	World.Explode(Bomb.X, Bomb.Y, Bomb.Z, CubeSpec::CraterPower, BlastSeed(Bomb.Id), Bomb.Holder, ServerName, Region, Update);
	Publish(Update);
}

void ACubeWorldGameMode::ShareBomb(const FCubeBombRecord& Bomb, bool bOwned)
{
	if (bOffline) { OnBomb(Bomb, bOwned); return; }
	UWorldBomb* Row = PlayServ::Data::Create<UWorldBomb>();
	Row->bomb_id = Bomb.Id; Row->state = Bomb.State; Row->holder = Bomb.Holder;
	Row->x = Bomb.X; Row->y = Bomb.Y; Row->z = Bomb.Z; Row->vx = Bomb.VX; Row->vy = Bomb.VY; Row->vz = Bomb.VZ;
	Row->dropped_at = Bomb.DroppedAt; Row->at = Bomb.At;
	TStrongObjectPtr<UWorldBomb> Keep(Row);
	TWeakObjectPtr<ACubeWorldGameMode> Weak(this);
	PlayServ::Data::Save(Row, FPlayServSimpleCallback::CreateLambda([Weak, Keep](bool bOk, const FPlayServError& Error)
	{
		if (Weak.IsValid() && !bOk) Weak->ServerLog(FString::Printf(TEXT("bomb not written: %s"), *Error.Message));
	}));
	OnBomb(Bomb, bOwned);
}

/** A bomb moved on, here or on another server. Anything that does not move it forward is an echo or stale. */
void ACubeWorldGameMode::OnBomb(const FCubeBombRecord& Bomb, bool bOwned)
{
	// A bomb that went off or fizzled stays over. Every push of the live table brings its rows round again, its free row
	// among them, and taking that row brought the bomb back as a ghost: a free bomb only this server had, handed to
	// its clients on every push and on every join, that a player could pick up and carry into a C# room, which knew it
	// was gone. The C# servers keep the same rule (CubeWorldServer.OnBomb, FinishedBombs).
	const int64 NowMs = Now();
	if (const int64* Ended = BombsOver.Find(Bomb.Id))
	{
		if (NowMs - *Ended < BombsOverKeepMs) { Bombs.Remove(Bomb.Id); return; }
		BombsOver.Remove(Bomb.Id);
	}
	const FCubeLiveBomb* Known = Bombs.Find(Bomb.Id);
	if (Known && FCubeBombRecord::Rank(Bomb.State) <= FCubeBombRecord::Rank(Known->Record.State)) return;
	if (!Known && Bomb.IsOver() && NowMs - Bomb.At > 5000) { BombsOver.Add(Bomb.Id, Bomb.At); return; }
	// A record heard of a bomb this server does not follow, still in play though it was dropped long ago: a server that
	// missed its end handed it out again. Every server hears a bomb from its drop on, so a live bomb is known (Bomb.IsGhost).
	if (!Known && !Bomb.IsOver() && bBombsLoaded && NowMs - Bomb.DroppedAt > BombsOverKeepMs) return;
	if (Bomb.IsOver())
	{
		// Always let go of it: a bomb left in Bombs as flying explodes again every tick.
		Bombs.Remove(Bomb.Id);
		BombsOver.Add(Bomb.Id, NowMs);
		if (BombsOver.Num() > 2000) for (auto It = BombsOver.CreateIterator(); It; ++It) if (NowMs - It.Value() >= BombsOverKeepMs) It.RemoveCurrent();
		ServerLog(FString::Printf(TEXT("bomb %s %s at %.1f %.1f %.1f (%s)"), *Bomb.Id, *Bomb.State, Bomb.X, Bomb.Y, Bomb.Z, *Bomb.Holder));
	}

	if (Bomb.IsOver()) Bombs.Remove(Bomb.Id);
	else
	{
		FCubeLiveBomb Live;
		Live.Record = Bomb;
		Live.Z = Bomb.Z;
		// A free bomb heard late is brought down as far as it has come since it was dropped.
		if (Bomb.State == TEXT("free"))
		{
			const int64 Ticks = FMath::Clamp<int64>((Now() - Bomb.At) / (1000 / CubeSpec::TicksPerSecond), 0, 2000);
			for (int64 T = 0; T < Ticks; T++) Live.Z = CubeBombs::Descend(World.Voxels, Bomb.X, Bomb.Y, Live.Z);
		}
		Live.bOwned = bOwned && Bomb.State == TEXT("flying");
		Live.P[0] = Bomb.X; Live.P[1] = Bomb.Y; Live.P[2] = Bomb.Z;
		Live.V[0] = Bomb.VX; Live.V[1] = Bomb.VY; Live.V[2] = Bomb.VZ;
		Bombs.Add(Bomb.Id, Live);
	}
	if (Bomb.State == TEXT("exploded")) Crater(Bomb);

	for (auto& Pair : Players)
	{
		FCubeServerPlayer& P = *Pair.Value;
		if (P.Bomb == Bomb.Id && (Bomb.State != TEXT("held") || Bomb.Holder != P.Id)) P.Bomb.Empty();
	}
	if (Bomb.State == TEXT("held")) if (FCubeServerPlayer* Holder = PlayerById(Bomb.Holder)) Holder->Bomb = Bomb.Id;
	BroadcastBomb(BombFrame(Bomb, Bombs.Find(Bomb.Id)));
}

// ── platform data: writes ────────────────────────────────────────────────────────────────────────

/**
 * Writes a block this server changed. The row is updated when known; a block never written from here is created,
 * and a create that collides with a row another server made in the same moment finds that row and updates it.
 */
void ACubeWorldGameMode::WriteCube(const FIntVector& At, int32 Attempt)
{
	if (bOffline) return;
	FCubeOverride* O = World.Overrides.Find(At);
	if (!O) { if (Attempt > 0) WriteDone(At, false); return; }
	// The write is on its way until it lands or is given up, through every attempt.
	if (Attempt == 0) WritesInFlight.FindOrAdd(At)++;
	UWorldCube* Row = O->Row.Get();
	if (!Row)
	{
		Row = PlayServ::Data::Create<UWorldCube>();
		Row->key = FCubeServerWorld::Key(At.X, At.Y, At.Z);
		Row->x = At.X; Row->y = At.Y; Row->z = At.Z;
		O->Row.Reset(Row);
	}
	Row->kind = CubeSpec::KindName(O->Kind);
	Row->placed_by = O->By;
	Row->placed_on = O->On;
	Row->at = O->At = Now();
	TWeakObjectPtr<ACubeWorldGameMode> Weak(this);
	PlayServ::Data::Save(Row, FPlayServSimpleCallback::CreateLambda([Weak, At, Attempt](bool bOk, const FPlayServError& Error)
	{
		if (!Weak.IsValid()) return;
		ACubeWorldGameMode* Self = Weak.Get();
		if (bOk) { Self->WriteDone(At, true); return; }
		if (Attempt >= 2) { Self->ServerLog(FString::Printf(TEXT("block %d %d %d not written: %s"), At.X, At.Y, At.Z, *Error.Message)); Self->WriteDone(At, false); return; }
		// Someone else wrote this block first: take their row and write ours over it.
		PlayServ::Data::LoadAll<UWorldCube>(FPlayServFilter::Where(TEXT("key")).EqualTo(FCubeServerWorld::Key(At.X, At.Y, At.Z)), [Weak, At, Attempt](bool bFound, TArray<UWorldCube*> Rows, const FPlayServError&)
		{
			if (!Weak.IsValid()) return;
			if (bFound && Rows.Num() > 0) Weak->World.Remember(At, Rows[0]);
			else if (FCubeOverride* Again = Weak->World.Overrides.Find(At)) Again->Row.Reset();
			Weak->WriteCube(At, Attempt + 1);
		});
	}));
}

void ACubeWorldGameMode::WriteDone(const FIntVector& At, bool bLanded)
{
	if (int32* InFlight = WritesInFlight.Find(At)) { if (--*InFlight <= 0) WritesInFlight.Remove(At); }
	if (!bLanded) return;
	// In the table from now on: a read of it that began before this does not take the block for gone.
	World.Touch(At);
	if (CubesSubscribedAt > 0 && CubeWriteLandedAt == 0) CubeWriteLandedAt = Now();
}

void ACubeWorldGameMode::WritePresence(FCubeServerPlayer& P)
{
	if (bOffline) { P.bMoved = false; P.PresenceWrittenAt = Now(); return; }
	if (P.bPresenceBusy) return;
	P.bPresenceBusy = true;
	P.bMoved = false;
	P.PresenceWrittenAt = Now();
	TWeakObjectPtr<ACubeWorldGameMode> Weak(this);
	const FString Id = P.Id;
	auto Write = [Weak, Id]()
	{
		if (!Weak.IsValid()) return;
		FCubeServerPlayer* P = Weak->PlayerById(Id);
		if (!P || !P->PresenceRow.IsValid()) { if (P) P->bPresenceBusy = false; return; }
		UWorldPresence* Row = P->PresenceRow.Get();
		Row->player_id = P->Id; Row->name = P->Name; Row->server = Weak->ServerName; Row->color = Weak->Color();
		Row->x = P->X; Row->y = P->Y; Row->z = P->Z; Row->yaw = P->Yaw; Row->pitch = P->Pitch; Row->health = P->Health;
		Row->sneaking = P->bSneaking ? 1 : 0; Row->sprinting = P->bSprinting ? 1 : 0; Row->seen_at = Now();
		PlayServ::Data::Save(Row, FPlayServSimpleCallback::CreateLambda([Weak, Id](bool bOk, const FPlayServError& Error)
		{
			if (!Weak.IsValid()) return;
			FCubeServerPlayer* P = Weak->PlayerById(Id);
			if (!P) return;
			P->bPresenceBusy = false;
			if (bOk) return;
			// The row is gone (another server took the player over and deleted it): find or make one next time. A refused
			// version (a read in flight during the write) rights itself on the next read, so the row is kept.
			if (Error.Code == EPlayServErrorCode::NotFound) P->PresenceRow.Reset();
			P->bMoved = true;
		}));
	};
	if (P.PresenceRow.IsValid()) { Write(); return; }
	// The player's row from wherever they last were, or a new one.
	PlayServ::Data::LoadAll<UWorldPresence>(FPlayServFilter::Where(TEXT("player_id")).EqualTo(Id), [Weak, Id, Write](bool bOk, TArray<UWorldPresence*> Rows, const FPlayServError&)
	{
		if (!Weak.IsValid()) return;
		FCubeServerPlayer* P = Weak->PlayerById(Id);
		if (!P) return;
		P->PresenceRow.Reset(bOk && Rows.Num() > 0 ? Rows[0] : PlayServ::Data::Create<UWorldPresence>());
		Write();
	});
}

// A player who left: their row goes, unless another server already took them over (a border crossing).
void ACubeWorldGameMode::DeletePresence(const FString& PlayerId, UWorldPresence* Row)
{
	if (bOffline || Row->server != ServerName) return;
	TStrongObjectPtr<UWorldPresence> Keep(Row);
	TWeakObjectPtr<ACubeWorldGameMode> Weak(this);
	PlayServ::Data::Reload(Row, FPlayServSimpleCallback::CreateLambda([Weak, Keep, PlayerId](bool bOk, const FPlayServError&)
	{
		if (!Weak.IsValid() || !bOk || Keep->server != Weak->ServerName || Weak->Players.Contains(PlayerId)) return;
		PlayServ::Data::Delete(Keep.Get(), FPlayServSimpleCallback::CreateLambda([Keep](bool, const FPlayServError&) {}));
	}));
}

void ACubeWorldGameMode::WriteInventory(FCubeServerPlayer& P)
{
	UCubeInventory* Row = P.InventoryRow.Get();
	if (bOffline || !Row) return;
	Row->player_id = P.Id;
	Row->cubes = P.Inventory.Total();
	Row->stacks = P.Inventory.ToJson();
	P.InventorySync.Wrote(P.Inventory, Now());
	TWeakObjectPtr<ACubeWorldGameMode> Weak(this);
	TStrongObjectPtr<UCubeInventory> Keep(Row);
	const FString Id = P.Id;
	const FCubeInventory Sent = P.Inventory;
	PlayServ::Data::Save(Row, FPlayServSimpleCallback::CreateLambda([Weak, Keep, Id, Sent](bool bOk, const FPlayServError& Error)
	{
		if (!Weak.IsValid()) return;
		ACubeWorldGameMode* Self = Weak.Get();
		FCubeServerPlayer* P = Self->PlayerById(Id);
		if (bOk) { if (P) P->InventoryConflicts = 0; return; }
		// A write that never landed never comes back.
		if (P)
		{
			const int32 Unsent = P->InventorySync.Written.IndexOfByPredicate([&Sent](const TPair<FCubeInventory, int64>& W) { return W.Key.Same(Sent); });
			if (Unsent != INDEX_NONE) P->InventorySync.Written.RemoveAt(Unsent);
		}
		// Another writer changed the row since this server read it (the old server's last write, a refill): the save is
		// refused, and was never retried, so every later save of this player failed too. The row is read again, the
		// other writer's change merged in, and the result saved.
		if (Error.Code == EPlayServErrorCode::PreconditionFailed && P && P->InventoryRow.Get() == Keep.Get() && ++P->InventoryConflicts <= 5)
		{
			PlayServ::Data::Reload(Keep.Get(), FPlayServSimpleCallback::CreateLambda([Weak, Keep, Id](bool bReloaded, const FPlayServError& ReloadError)
			{
				if (!Weak.IsValid()) return;
				ACubeWorldGameMode* Inner = Weak.Get();
				FCubeServerPlayer* Q = Inner->PlayerById(Id);
				if (!Q || Q->InventoryRow.Get() != Keep.Get()) return;
				if (!bReloaded) { Inner->ServerLog(FString::Printf(TEXT("inventory of %s not read again: %s"), *Id, *ReloadError.Message)); return; }
				FCubeInventory Merged;
				const FCubeInventory Theirs = FCubeInventory::Parse(Keep->stacks);
				const bool bForeign = Q->InventorySync.Heard(Q->Inventory, Theirs, Inner->Now(), Merged);
				const bool bChangedHere = bForeign && !Merged.Same(Q->Inventory);
				if (bForeign) Q->Inventory = Merged;
				Inner->WriteInventory(*Q);
				if (bChangedHere) Inner->SendInventory(*Q);
			}));
			return;
		}
		Self->ServerLog(FString::Printf(TEXT("inventory of %s not written: %s"), *Keep->player_id, *Error.Message));
	}));
}

void ACubeWorldGameMode::WriteHit(const FString& HitId, const FString& Victim, const FString& Attacker, double Damage, double KX, double KY, double Strength)
{
	if (bOffline) return;
	UWorldHit* Row = PlayServ::Data::Create<UWorldHit>();
	Row->hit_id = HitId; Row->victim = Victim; Row->attacker = Attacker;
	Row->damage = Damage; Row->kx = KX; Row->ky = KY; Row->strength = Strength; Row->at = Now();
	TStrongObjectPtr<UWorldHit> Keep(Row);
	TWeakObjectPtr<ACubeWorldGameMode> Weak(this);
	PlayServ::Data::Save(Row, FPlayServSimpleCallback::CreateLambda([Weak, Keep](bool bOk, const FPlayServError& Error)
	{
		if (Weak.IsValid() && !bOk) Weak->ServerLog(FString::Printf(TEXT("hit not written: %s"), *Error.Message));
	}));
}

// ── platform data: what the other servers wrote ──────────────────────────────────────────────────

void ACubeWorldGameMode::PollCubes()
{
	const int64 T = Now();
	// A late row is at most a few seconds behind its delete; a minute on, the delete has no older row left to stop.
	for (auto It = Tombstones.CreateIterator(); It; ++It) if (T - It->Value.HeardAt > TombstoneTtlMs) It.RemoveCurrent();
	// This server's own writes come back over the uplink like anyone's: one that landed a minute ago with nothing come
	// back says the platform sends this uplink no changes, and then a delete (the world's reset) is heard only at the
	// next restart.
	if (!bCubeUpdatesHeard && !bCubeUpdatesWarned && CubeWriteLandedAt > 0 && T - CubeWriteLandedAt > 60000)
	{
		bCubeUpdatesWarned = true;
		UE_LOG(LogCubeWorld, Warning, TEXT("%s: nothing has come over the uplink's WorldCube subscription in the minute since this server's own write landed; if the platform sends this uplink no data_update, deleted blocks (the world's reset) are heard only when the server restarts"), *RoomName());
	}
	if (bCubesBusy) return;
	bCubesBusy = true;
	TWeakObjectPtr<ACubeWorldGameMode> Weak(this);
	PlayServ::Data::LoadAll<UWorldCube>(FPlayServFilter::Where(TEXT("at")).GreaterThan((double)(LastCubeAt - 1)), [Weak](bool bOk, TArray<UWorldCube*> Rows, const FPlayServError& Error)
	{
		if (!Weak.IsValid()) return;
		ACubeWorldGameMode* Self = Weak.Get();
		Self->bCubesBusy = false;
		if (!bOk) { Self->ServerLog(FString::Printf(TEXT("cubes not read: %s"), *Error.Message)); return; }
		for (UWorldCube* Row : Rows)
		{
			Self->LastCubeAt = FMath::Max(Self->LastCubeAt, Row->at);
			const FIntVector At(Row->x, Row->y, Row->z);
			// A poll answered before a delete can come back after the uplink brought the delete.
			if (Self->IsBuried(At, Row->at)) continue;
			const bool bChanged = Self->World.Apply(At, FName(*Row->kind), Row->placed_by, Row->placed_on, Row->at, Row);
			if (bChanged && Row->placed_on != Self->ServerName) Self->Heard.Add({ At, FName(*Row->kind), Row->placed_by, Row->placed_on });
		}
	});
}

void ACubeWorldGameMode::PollPresence()
{
	if (bPresenceBusy) return;
	bPresenceBusy = true;
	TWeakObjectPtr<ACubeWorldGameMode> Weak(this);
	PlayServ::Data::LoadAll<UWorldPresence>(FPlayServFilter::Where(TEXT("seen_at")).GreaterThan((double)(Now() - PresenceTtlMs)), [Weak](bool bOk, TArray<UWorldPresence*> Rows, const FPlayServError& Error)
	{
		if (!Weak.IsValid()) return;
		ACubeWorldGameMode* Self = Weak.Get();
		Self->bPresenceBusy = false;
		if (!bOk) { Self->ServerLog(FString::Printf(TEXT("presence not read: %s"), *Error.Message)); return; }
		TMap<FString, FCubeElsewhere> Fresh;
		for (const UWorldPresence* Row : Rows)
		{
			if (Row->server == Self->ServerName || Self->Players.Contains(Row->player_id)) continue;
			FCubeElsewhere E;
			E.Pose.Id = Row->player_id; E.Pose.Name = Row->name; E.Pose.Server = Row->server; E.Pose.Color = Row->color;
			E.Pose.X = Row->x; E.Pose.Y = Row->y; E.Pose.Z = Row->z; E.Pose.Yaw = Row->yaw; E.Pose.Pitch = Row->pitch; E.Pose.Health = Row->health;
			E.Pose.bSneaking = Row->sneaking == 1; E.Pose.bSprinting = Row->sprinting == 1;
			E.SeenAt = Row->seen_at;
			const FCubeElsewhere* Known = Fresh.Find(Row->player_id);
			if (!Known || Row->seen_at > Known->SeenAt) Fresh.Add(Row->player_id, E);
		}
		Self->Elsewhere = Fresh;
	});
}

void ACubeWorldGameMode::PollHits()
{
	if (bHitsBusy || Players.Num() == 0) return;
	bHitsBusy = true;
	TWeakObjectPtr<ACubeWorldGameMode> Weak(this);
	PlayServ::Data::LoadAll<UWorldHit>(FPlayServFilter::Where(TEXT("at")).GreaterThan((double)(Now() - 5000)), [Weak](bool bOk, TArray<UWorldHit*> Rows, const FPlayServError& Error)
	{
		if (!Weak.IsValid()) return;
		ACubeWorldGameMode* Self = Weak.Get();
		Self->bHitsBusy = false;
		if (!bOk) { Self->ServerLog(FString::Printf(TEXT("hits not read: %s"), *Error.Message)); return; }
		for (UWorldHit* Row : Rows)
		{
			FCubeServerPlayer* Victim = Self->PlayerById(Row->victim);
			if (!Victim || Self->HitsApplied.Contains(Row->hit_id)) continue;
			Self->HitsApplied.Add(Row->hit_id);
			Self->Hurt(*Victim, Row->damage, true, Row->kx, Row->ky, Row->strength, Row->attacker);
			TStrongObjectPtr<UWorldHit> Keep(Row);
			PlayServ::Data::Delete(Row, FPlayServSimpleCallback::CreateLambda([Keep](bool, const FPlayServError&) {}));
		}
		if (Self->HitsApplied.Num() > 1000) Self->HitsApplied.Empty();
	});
}

void ACubeWorldGameMode::PollBombs()
{
	if (bBombsBusy) return;
	bBombsBusy = true;
	TWeakObjectPtr<ACubeWorldGameMode> Weak(this);
	PlayServ::Data::LoadAll<UWorldBomb>(FPlayServFilter::Where(TEXT("at")).GreaterThan((double)(LastBombAt - 1)), [Weak](bool bOk, TArray<UWorldBomb*> Rows, const FPlayServError& Error)
	{
		if (!Weak.IsValid()) return;
		ACubeWorldGameMode* Self = Weak.Get();
		Self->bBombsBusy = false;
		if (!bOk) { Self->ServerLog(FString::Printf(TEXT("bombs not read: %s"), *Error.Message)); return; }
		Rows.Sort([](const UWorldBomb& A, const UWorldBomb& B) { return A.at < B.at; });
		for (const UWorldBomb* Row : Rows)
		{
			Self->LastBombAt = FMath::Max(Self->LastBombAt, Row->at);
			Self->OnBomb(RecordOf(Row), false);
		}
	});
}

void ACubeWorldGameMode::PollRegions()
{
	if (bRegionsBusy) return;
	bRegionsBusy = true;
	TWeakObjectPtr<ACubeWorldGameMode> Weak(this);
	PlayServ::Data::LoadAll<UWorldRegion>(FPlayServFilter::None(), [Weak](bool bOk, TArray<UWorldRegion*> Rows, const FPlayServError&)
	{
		if (!Weak.IsValid()) return;
		ACubeWorldGameMode* Self = Weak.Get();
		Self->bRegionsBusy = false;
		if (!bOk) return;
		TArray<FCubeRegionRep> Live;
		const int64 T = Now();
		for (const UWorldRegion* Row : Rows)
			if (T - Row->seen_at < RegionTtlMs) Live.Add({ FCString::Atoi(*Row->region), Row->room, Row->color, Row->server, Row->slug });
		Live.Sort([](const FCubeRegionRep& A, const FCubeRegionRep& B) { return A.Region < B.Region; });
		Self->Regions = Live;
		if (Self->State) Self->State->Regions = Live;
	});
}

// ── the platform's rooms ─────────────────────────────────────────────────────────────────────────

void ACubeWorldGameMode::TurnAway(FCubeServerPlayer& P, const FString& Reason)
{
	// A browser client hears the reason on the close, as the C# server says it.
	if (P.WebClient) { if (Web.IsValid()) Web->Close(P.WebClient, 1008, Reason); return; }
	ACubePlayerPawn* Pawn = P.Pawn.Get();
	if (!Pawn) return;
	Pawn->ClientTurnedAway(Reason);
	TWeakObjectPtr<ACubePlayerPawn> WeakPawn(Pawn);
	TWeakObjectPtr<ACubeWorldGameMode> Weak(this);
	FTimerHandle H;
	GetWorldTimerManager().SetTimer(H, [Weak, WeakPawn, Reason]()
	{
		if (!Weak.IsValid() || !WeakPawn.IsValid()) return;
		if (APlayerController* PC = Cast<APlayerController>(WeakPawn->GetController()))
			if (Weak->GameSession) Weak->GameSession->KickPlayer(PC, FText::FromString(Reason));
	}, 0.5f, false);
}

/**
 * The room was closed by the platform: the operator closed it, or it reached its lifetime. Its region goes back to
 * the generated terrain (every change in it is deleted, and every server and client hears the deletes), the bombs
 * over it go up in smoke, and the process ends. Docker starts it again on the same machine, where it claims its
 * region again and opens the room fresh.
 */
void ACubeWorldGameMode::HandleRoomEnded(const FString& InRoomName, const FString& Reason)
{
	// A room refused at registration (another process still holds the name) was never ours to clear.
	if (InRoomName != RoomName() || bClosing || !bServing) return;
	bClosing = true;
	ServerLog(FString::Printf(TEXT("was closed (%s): clearing region %d and exiting"), *Reason, Region));
	for (auto& Pair : Players) TurnAway(*Pair.Value, TEXT("room_closed_by_operator"));
	FTimerHandle H;
	GetWorldTimerManager().SetTimer(H, this, &ACubeWorldGameMode::ClearRegionAndExit, 1.f, false);
}

void ACubeWorldGameMode::ClearRegionAndExit()
{
	TArray<FString> Ids;
	Bombs.GetKeys(Ids);
	for (const FString& Id : Ids)
	{
		const FCubeLiveBomb* Live = Bombs.Find(Id);
		if (Live && !Live->Record.IsOver() && CubeSpec::RegionOf(Live->Record.X, Live->Record.Y) == Region)
			ShareBomb(Next(Live->Record, TEXT("fizzled"), Live->Record.Holder, Live->Record.X, Live->Record.Y, Live->Z), false);
	}
	// The rows of the region go a few at a time: the SDK's DeleteAll fires every delete at once, and hundreds of
	// requests in flight run past its timeout.
	int32 X0, X1, Y0, Y1;
	CubeSpec::RegionBounds(Region, X0, X1, Y0, Y1);
	TWeakObjectPtr<ACubeWorldGameMode> Weak(this);
	PlayServ::Data::LoadAll<UWorldCube>(FPlayServFilter::Where(TEXT("x")).GreaterThanOrEqual(X0).And(TEXT("x")).LessThan(X1).And(TEXT("y")).GreaterThanOrEqual(Y0).And(TEXT("y")).LessThan(Y1), [Weak](bool bOk, TArray<UWorldCube*> Rows, const FPlayServError& Error)
	{
		if (!Weak.IsValid()) return;
		if (!bOk) { Weak->ServerLog(FString::Printf(TEXT("region not cleared, its rows could not be read: %s"), *Error.Message)); Weak->ExitSoon(); return; }
		TSharedRef<TArray<TStrongObjectPtr<UWorldCube>>> Pending = MakeShared<TArray<TStrongObjectPtr<UWorldCube>>>();
		for (UWorldCube* Row : Rows) Pending->Add(TStrongObjectPtr<UWorldCube>(Row));
		const int32 Total = Pending->Num();
		TSharedRef<int32> Deleted = MakeShared<int32>(0), Failed = MakeShared<int32>(0), InFlight = MakeShared<int32>(0);
		TSharedRef<TFunction<void()>> Next = MakeShared<TFunction<void()>>();
		*Next = [Weak, Pending, Total, Deleted, Failed, InFlight, Next]()
		{
			while (Pending->Num() > 0 && *InFlight < 16)
			{
				TStrongObjectPtr<UWorldCube> Row = Pending->Pop();
				(*InFlight)++;
				PlayServ::Data::Delete(Row.Get(), FPlayServSimpleCallback::CreateLambda([Weak, Row, Total, Deleted, Failed, InFlight, Next](bool bDeleted, const FPlayServError&)
				{
					(*InFlight)--;
					if (bDeleted) (*Deleted)++; else (*Failed)++;
					if (*Deleted + *Failed < Total) { (*Next)(); return; }
					if (!Weak.IsValid()) return;
					Weak->ServerLog(FString::Printf(TEXT("region cleared, %d changed blocks deleted%s"), *Deleted, *Failed > 0 ? *FString::Printf(TEXT(", %d not"), *Failed) : TEXT("")));
					Weak->ExitSoon();
				}));
			}
		};
		if (Total == 0) { Weak->ServerLog(TEXT("region cleared, nothing to delete")); Weak->ExitSoon(); return; }
		(*Next)();
	});
}

// The platform has its answer and the players their reason already; this gives the logs time to leave before the process does.
void ACubeWorldGameMode::ExitSoon()
{
	FTimerHandle H;
	GetWorldTimerManager().SetTimer(H, []() { PlayServ::Rooms::StopHosting(); FPlatformMisc::RequestExit(false); }, 3.f, false);
}

void ACubeWorldGameMode::HandlePlayerRemoved(const FString& InRoomName, const FString& PlayerId, const FString& Reason)
{
	if (InRoomName != RoomName()) return;
	FCubeServerPlayer* P = PlayerById(PlayerId);
	if (!P || Reason == TEXT("reconnect_grace_lapsed") || Reason == TEXT("removed_by_game")) return;
	ServerLog(FString::Printf(TEXT("%s removed by the platform (%s)"), *P->Name, *Reason));
	TurnAway(*P, TEXT("removed_by_operator"));
}

/**
 * The operator's Delete room reaches this SDK as the room's placement turning to session_closing on the next
 * heartbeat: the platform cannot ask the Unreal SDK to end the room outright (it answers `not_supported`), but the
 * server sees the state and ends the room itself.
 */
void ACubeWorldGameMode::HandleRoomPlacementChanged(const FString& InRoomName, EPlayServPlacementState Placement)
{
	if (Placement == EPlayServPlacementState::SessionClosing) HandleRoomEnded(InRoomName, TEXT("room_closed_by_operator"));
}
