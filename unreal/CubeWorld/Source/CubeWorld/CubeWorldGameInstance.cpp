#include "CubeWorldGameInstance.h"
#include "CubeWorld.h"
#include "CubeWorldActor.h"
#include "CubeSocket.h"
#include "PlayServ.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "TimerManager.h"

void UCubeWorldGameInstance::Init()
{
	Super::Init();
	if (!CubeIsServerProcess()) Textures.Build();
	Hotbar = CubeSpec::Hotbar();
	FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(this, &UCubeWorldGameInstance::HandlePostLoadMap);
	if (GEngine) GEngine->OnNetworkFailure().AddUObject(this, &UCubeWorldGameInstance::HandleNetworkFailure);
}

void UCubeWorldGameInstance::Shutdown()
{
	CloseSockets();
	FCoreUObjectDelegates::PostLoadMapWithWorld.RemoveAll(this);
	if (GEngine) GEngine->OnNetworkFailure().RemoveAll(this);
	Super::Shutdown();
}

// The world on screen lives on the client alone; the server has no picture to draw.
void UCubeWorldGameInstance::HandlePostLoadMap(UWorld* LoadedWorld)
{
	if (!LoadedWorld || CubeIsServerProcess() || LoadedWorld->GetNetMode() == NM_DedicatedServer) return;
	LoadedWorld->SpawnActor<ACubeWorldActor>();
	// Back in the local map after an Unreal server: the C# server waiting for us is reached now.
	if (SocketPlan.bSet && LoadedWorld->GetNetMode() == NM_Standalone)
	{
		const FSocketPlan Plan = SocketPlan;
		SocketPlan = FSocketPlan();
		ConnectSocket(Plan.RoomName, Plan.Host, Plan.Port, Plan.bSecure, Plan.ReservationToken, Plan.bTeleport);
	}
}

void UCubeWorldGameInstance::Log(const FString& Text)
{
	UE_LOG(LogCubeWorld, Log, TEXT("%s"), *Text);
	LogLines.Insert(Text, 0);
	if (LogLines.Num() > 8) LogLines.SetNum(8);
}

void UCubeWorldGameInstance::StartPlay(const FString& Name)
{
	if (bSigningIn || IsConnected() || bSwitching) return;
	PlayerName = Name;
	bSigningIn = true;
	Status = TEXT("Signing in...");
	if (PlayServ::Auth::IsLoggedIn() && !PlayerId.IsEmpty()) { Browse(); return; }
	TWeakObjectPtr<UCubeWorldGameInstance> Weak(this);
	PlayServ::Auth::LoginAnonymous(Name, FPlayServAuthCallback::CreateLambda([Weak](bool bOk, const FString& InPlayerId, const FPlayServError& Error)
	{
		if (!Weak.IsValid()) return;
		UCubeWorldGameInstance* Self = Weak.Get();
		if (!bOk)
		{
			Self->bSigningIn = false;
			Self->Status = FString::Printf(TEXT("Sign-in failed: %s"), *Error.Message);
			Self->Log(Self->Status);
			return;
		}
		Self->PlayerId = InPlayerId;
		Self->Log(FString::Printf(TEXT("signed in as %s"), *Self->PlayerName));
		Self->Browse();
	}));
}

// An operator's close is followed by the room opening again fresh within a minute or two; an operator's removal holds
// for as long as that room lives. The same rules as web/rooms.js.
FString UCubeWorldGameInstance::TurnedAway(const FString& RoomName, const FString& ReasonOrCode)
{
	double Wait = 0;
	FString Message;
	if (ReasonOrCode.Contains(TEXT("room_closed")))
	{
		Wait = 30;
		Message = TEXT("This room was closed by an operator. It opens again fresh in a minute or two.");
	}
	else if (ReasonOrCode.Contains(TEXT("removed")))
	{
		Wait = 60;
		Message = TEXT("An operator removed you from this room. You can still walk into the other regions.");
	}
	if (!Message.IsEmpty() && !RoomName.IsEmpty()) NotBefore.Add(RoomName, FPlatformTime::Seconds() + Wait);
	return Message;
}

void UCubeWorldGameInstance::TurnedAwayBy(const FString& Reason)
{
	const FString Message = TurnedAway(Room.IsEmpty() ? Travelling : Room, Reason);
	Log(Message.IsEmpty() ? FString::Printf(TEXT("turned away: %s"), *Reason) : Message);
}

void UCubeWorldGameInstance::Browse()
{
	if (BrowsesPending > 0) return;
	Status = TEXT("Looking for servers...");
	// The C# servers and the Unreal servers register under their own room types; both are listed, and a room is
	// joined under the type it was found in. A type the project has not got simply lists nothing.
	BrowseFound.Empty();
	BrowseError.Empty();
	BrowsesPending = CubeSpec::RoomTypes().Num();
	TWeakObjectPtr<UCubeWorldGameInstance> Weak(this);
	for (const FString& Slug : CubeSpec::RoomTypes())
	{
		PlayServ::Rooms::Browse(Slug, FPlayServRoomFilters(), FPlayServBrowseCallback::CreateLambda([Weak, Slug](bool bOk, const FPlayServBrowsePage& Page, const FPlayServError& Error)
		{
			if (!Weak.IsValid()) return;
			UCubeWorldGameInstance* Self = Weak.Get();
			if (bOk)
				for (const FPlayServRoomListing& R : Page.Rooms)
				{
					Self->RoomSlugs.Add(R.RoomName, Slug);
					if ((R.PlacementState == EPlayServPlacementState::Open || R.PlacementState == EPlayServPlacementState::Unknown) && Self->MayTry(R.RoomName)) Self->BrowseFound.AddUnique(R.RoomName);
				}
			else if (Error.ProblemCode != TEXT("room_type_not_found") && Error.ProblemCode != TEXT("not_found")) Self->BrowseError = Error.Message;
			if (--Self->BrowsesPending > 0) return;
			Self->Browsed();
		}));
	}
}

void UCubeWorldGameInstance::Browsed()
{
	TWeakObjectPtr<UCubeWorldGameInstance> Weak(this);
	if (BrowseFound.Num() == 0)
	{
		if (!BrowseError.IsEmpty() || RoomSlugs.Num() == 0)
		{
			bSigningIn = false;
			Status = BrowseError.IsEmpty() ? TEXT("No server is running. Press Enter to retry.") : FString::Printf(TEXT("Browse failed: %s"), *BrowseError);
			Log(Status);
			return;
		}
		Status = TEXT("Every server is closing or full, retrying...");
		GetTimerManager().SetTimer(RetryTimer, [Weak]() { if (Weak.IsValid()) Weak->Browse(); }, 3.f, false);
		return;
	}
	Candidates = BrowseFound;
	// The Unreal servers first, the C# ones after: the Unreal client is at home on Iris.
	Candidates.Sort([this](const FString& A, const FString& B)
	{
		const bool bUnrealA = RoomSlugs.FindRef(A) == TEXT("cubeworld-ue"), bUnrealB = RoomSlugs.FindRef(B) == TEXT("cubeworld-ue");
		return bUnrealA != bUnrealB ? bUnrealA : A < B;
	});
	const FString First = Candidates[0];
	Candidates.RemoveAt(0);
	Enter(First, true);
}

void UCubeWorldGameInstance::Enter(const FString& RoomName, bool bTeleport)
{
	if (RoomName == Room || bSwitching) return;
	bSwitching = true;
	Status = FString::Printf(TEXT("Joining %s..."), *RoomName);
	TWeakObjectPtr<UCubeWorldGameInstance> Weak(this);
	// The SDK is not handed the controller: the travel is ours, so the position survives a border crossing.
	const FPlayServJoinCallback Joined = FPlayServJoinCallback::CreateLambda([Weak, RoomName, bTeleport](bool bOk, const FPlayServJoinResult& Result, const FPlayServError& Error)
	{
		if (!Weak.IsValid()) return;
		UCubeWorldGameInstance* Self = Weak.Get();
		if (!bOk || !Result.Ticket.Connect.IsSet())
		{
			Self->bSwitching = false;
			Self->CrossAfter = FPlatformTime::Seconds() + 3;
			const FString Turned = Self->TurnedAway(RoomName, Error.ProblemCode);
			Self->Log(Turned.IsEmpty() ? FString::Printf(TEXT("%s refused the join: %s"), *RoomName, *Error.Message) : Turned);
			if (!Self->IsConnected())
			{
				if (Self->Candidates.Num() > 0) { const FString NextRoom = Self->Candidates[0]; Self->Candidates.RemoveAt(0); Self->Enter(NextRoom, bTeleport); }
				else Self->GetTimerManager().SetTimer(Self->RetryTimer, [Weak]() { if (Weak.IsValid()) Weak->Browse(); }, 3.f, false);
			}
			return;
		}
		const FPlayServRoomConnect& C = Result.Ticket.Connect;
		// An Unreal server says so in its attributes and is reached over Iris; any other room is a C# server, reached
		// over the JSON socket, plain or TLS (the SDK reads wss as anything but ws).
		if (Result.Ticket.Attributes.FindRef(TEXT("engine")) == TEXT("unreal"))
		{
			Self->TravelToUnrealServer(RoomName, PlayServ::Rooms::BuildTravelUrl(Result.Ticket), bTeleport);
			return;
		}
		FString Host = C.Host, Override;
		int32 Port = C.Port;
		bool bSecure = C.Transport != EPlayServRoomTransport::Ws;
		if (FParse::Param(FCommandLine::Get(), TEXT("wsplain"))) bSecure = false;
		if (FParse::Param(FCommandLine::Get(), TEXT("wssecure"))) bSecure = true;
		if (FParse::Value(FCommandLine::Get(), TEXT("-wshost="), Override) && !Override.IsEmpty())
		{
			FString PortText;
			if (Override.Split(TEXT(":"), &Host, &PortText)) Port = FCString::Atoi(*PortText); else Host = Override;
		}
		Self->ConnectSocket(RoomName, Host, Port, bSecure, Result.Ticket.ReservationToken, bTeleport);
	});
	// The room is joined under the type it was listed in; one heard of only by name goes under the default type.
	const FString Slug = RoomSlugs.FindRef(RoomName);
	if (Slug.IsEmpty()) PlayServ::Rooms::JoinRoom(RoomName, nullptr, Joined);
	else PlayServ::Rooms::JoinRoom(Slug, RoomName, nullptr, Joined);
}

void UCubeWorldGameInstance::TravelToUnrealServer(const FString& RoomName, const FString& Url, bool bTeleport)
{
	APlayerController* PC = GetFirstLocalPlayerController();
	if (!PC) { bSwitching = false; return; }
	CloseSockets();
	bViaSocket = false;
	// A crossing keeps the body where it is; a fresh join takes the server's spawn.
	Crossing = bTeleport ? FCubeCrossing() : LastBody;
	Crossing.bSet = !bTeleport && bPlaced;
	bWelcomed = false;
	bWorldLoaded = false;
	Travelling = RoomName;
	Players.Empty();
	Log(FString::Printf(TEXT("travelling to %s (Unreal) at %s"), *RoomName, *Url.Left(Url.Find(TEXT("?")) > 0 ? Url.Find(TEXT("?")) : Url.Len())));
	PC->ClientTravel(Url, ETravelType::TRAVEL_Absolute);
}

void UCubeWorldGameInstance::HandleNetworkFailure(UWorld* InWorld, UNetDriver* NetDriver, ENetworkFailure::Type FailureType, const FString& ErrorString)
{
	if (CubeIsServerProcess()) return;
	if (SocketPlan.bSet) return;   // leaving an Unreal server on purpose, for a C# one
	Disconnected(ErrorString.IsEmpty() ? FString(ENetworkFailure::ToString(FailureType)) : ErrorString);
}

// A kick (an operator's close or removal) brings the client back to the menu map; look for a server again after it.
void UCubeWorldGameInstance::ReturnToMainMenu()
{
	Super::ReturnToMainMenu();
	if (!CubeIsServerProcess()) Disconnected(TEXT("the server closed the connection"));
}

void UCubeWorldGameInstance::Disconnected(const FString& Why)
{
	if (!bWelcomed && !bSwitching && Travelling.IsEmpty()) return;
	if (Socket.IsValid()) { Socket->Close(); Socket.Reset(); }
	bViaSocket = false;
	const FString Turned = TurnedAway(Room.IsEmpty() ? Travelling : Room, Why);
	Log(Turned.IsEmpty() ? FString::Printf(TEXT("disconnected: %s"), *Why) : Turned);
	bWelcomed = false;
	bSwitching = false;
	bSigningIn = false;
	Room.Empty();
	Travelling.Empty();
	CrossAfter = FPlatformTime::Seconds() + 3;
	Reconnect();
}

// After the connection goes: look for a server again in a moment, keeping the sign-in.
void UCubeWorldGameInstance::Reconnect()
{
	Status = TEXT("Disconnected, reconnecting...");
	TWeakObjectPtr<UCubeWorldGameInstance> Weak(this);
	GetTimerManager().SetTimer(RetryTimer, [Weak]() { if (Weak.IsValid() && !Weak->IsConnected() && !Weak->bSwitching && !Weak->PlayerId.IsEmpty()) { Weak->bSigningIn = true; Weak->Browse(); } }, 3.f, false);
}

FString UCubeWorldGameInstance::RoomOfRegion(int32 InRegion) const
{
	for (const FCubeRegion& R : Regions) if (R.Region == InRegion) return R.Room;
	return FString();
}

void UCubeWorldGameInstance::MaybeCross(double X, double Y)
{
	if (!bPlaced || bSwitching || !IsConnected() || FPlatformTime::Seconds() < CrossAfter) return;
	const FString Here = RoomOfRegion(CubeSpec::RegionOf(X, Y));
	if (!Here.IsEmpty() && Here != Room && MayTry(Here)) Enter(Here, false);
}

FString UCubeWorldGameInstance::NameOf(const FString& Id) const
{
	if (Id == PlayerId) return TEXT("you");
	for (const FCubePresence& P : Players) if (P.Id == Id) return P.Name;
	return Id;
}

// ── what the server tells this client ────────────────────────────────────────────────────────────

void UCubeWorldGameInstance::OnWelcomed(const FString& InServer, const FString& InColor, const FString& InRoom, int32 InRegion, const FCubePose& You, const TArray<FCubeStackRep>& Stacks, int32 ChunkCount)
{
	const bool bCrossed = Crossing.bSet;
	Server = InServer; Color = InColor; Room = InRoom; Region = InRegion;
	Travelling.Empty();
	bSwitching = false;
	bSigningIn = false;
	bWelcomed = true;
	Health = You.Health;
	bDead = false;
	Status.Empty();
	World.Clear();
	bWorldLoaded = false;
	ChunksExpected = ChunkCount;
	ChunksReceived = 0;
	PendingCubes.Empty();
	SetInventory(Stacks);
	// The server placed the player where the hello asked: at the crossing point, or at its spawn.
	WelcomePose = You;
	Crossing = FCubeCrossing();
	Log(FString::Printf(TEXT("%s %s"), bCrossed ? TEXT("crossed into") : TEXT("entered"), *InRoom));
	if (ChunkCount == 0) OnWorldChunk(TArray<FCubeCellRep>(), true);
}

// The body is placed only once the whole world is here: it must not fall through blocks that have not arrived.
void UCubeWorldGameInstance::OnWorldChunk(const TArray<FCubeCellRep>& Cells, bool bLast)
{
	for (const FCubeCellRep& C : Cells) World.Set(C.X, C.Y, C.Z, CubeSpec::KindOf(C.Kind));
	ChunksReceived++;
	if (!bLast) return;
	bWorldLoaded = true;
	OnWelcome.Broadcast(WelcomePose, true);
	bPlaced = true;
	for (const FPendingCubes& P : PendingCubes) ApplyCubes(P.Changes, P.Falls, P.bRemote);
	PendingCubes.Empty();
}

void UCubeWorldGameInstance::ApplyCubes(const TArray<FCubeChangeRep>& Changes, const TArray<FCubeFallRep>& Falls, bool bRemote)
{
	if (!bWorldLoaded) { PendingCubes.Add({ Changes, Falls, bRemote }); return; }
	for (const FCubeFallRep& F : Falls) OnFall.Broadcast(CubeSpec::KindOf(F.Kind), F.X, F.Y, F.FromZ, F.ToZ);
	TArray<FIntVector> Changed;
	FString On;
	for (const FCubeChangeRep& C : Changes)
	{
		const FName Kind = CubeSpec::KindOf(C.Kind);
		World.Set(C.X, C.Y, C.Z, Kind);
		Changed.Add(FIntVector(C.X, C.Y, C.Z));
		OnCube.Broadcast(C.X, C.Y, C.Z, Kind);
		if (On.IsEmpty()) On = C.On;
	}
	OnCubes.Broadcast(Changed);
	if (Changed.Num() > 0 && bRemote)
		Log(FString::Printf(TEXT("%d block%s changed on server %s -> arrived here"), Changed.Num(), Changed.Num() > 1 ? TEXT("s") : TEXT(""), *On));
}

void UCubeWorldGameInstance::SetInventory(const TArray<FCubeStackRep>& Stacks)
{
	Inventory.Empty();
	for (const FCubeStackRep& S : Stacks) Inventory.Add(CubeSpec::KindOf(S.Kind), S.Count);
	OnInventory.Broadcast();
}

void UCubeWorldGameInstance::SetPlayers(const TArray<FCubePresenceRep>& InPlayers)
{
	Players.Empty();
	for (const FCubePresenceRep& R : InPlayers)
	{
		FCubePresence P;
		P.Id = R.Id; P.Name = R.Name; P.Server = R.Server; P.Color = R.Color;
		P.X = R.X; P.Y = R.Y; P.Z = R.Z; P.Yaw = R.Yaw; P.Pitch = R.Pitch; P.Health = R.Health;
		P.bSneaking = R.bSneaking; P.bSprinting = R.bSprinting;
		Players.Add(P);
	}
	OnPlayers.Broadcast(Players);
}

void UCubeWorldGameInstance::SetRegions(const TArray<FCubeRegionRep>& InRegions)
{
	Regions.Empty();
	for (const FCubeRegionRep& R : InRegions)
	{
		Regions.Add({ R.Region, R.Room, R.Color, R.Server, R.Slug });
		if (!R.Slug.IsEmpty()) RoomSlugs.Add(R.Room, R.Slug);
	}
}

void UCubeWorldGameInstance::OnHurtFrame(const FString& InPlayerId, double InHealth, double KX, double KY, double Strength)
{
	if (InPlayerId == PlayerId) Health = InHealth;
	OnHurt.Broadcast(InPlayerId, InHealth, KX, KY, Strength);
}

void UCubeWorldGameInstance::OnDeathFrame(const FString& InPlayerId, const FString& By)
{
	if (InPlayerId == PlayerId) { bDead = true; Health = 0; }
	OnDeath.Broadcast(InPlayerId, By);
}

void UCubeWorldGameInstance::OnRespawnFrame(const FCubePose& You)
{
	bDead = false;
	Health = You.Health;
	OnRespawn.Broadcast(You);
}

void UCubeWorldGameInstance::OnBombFrame(const FCubeBombRep& B)
{
	FCubeBombFrame F;
	F.Id = B.Id; F.State = B.State; F.Holder = B.Holder;
	F.X = B.X; F.Y = B.Y; F.Z = B.Z; F.VX = B.VX; F.VY = B.VY; F.VZ = B.VZ;
	F.Age = B.AgeMs;
	if (B.bHasHeight) F.Height = B.Height;
	OnBomb.Broadcast(F);
}
