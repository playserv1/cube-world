// The JSON socket to a C# server: the browser client's protocol (web/app.js), spoken over CubeSocket. The frames
// land in the same handlers the Unreal server's replication feeds, so the world, the pawn and the HUD see one game
// whichever server the player stands on. Leaving an Unreal server for a C# one keeps the world: the socket connects
// while the player still plays on the Unreal server, which is left when the C# one welcomes them (UCubeGameEngine).
#include "CubeWorldGameInstance.h"
#include "CubeWorld.h"
#include "CubeSocket.h"
#include "CubeSpec.h"
#include "PlayServ.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "CubeGameEngine.h"

namespace
{
	FString ToText(const TSharedRef<FJsonObject>& Object)
	{
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Object, Writer);
		return Out;
	}

	double Num(const TSharedPtr<FJsonObject>& O, const TCHAR* Field, double Default = 0)
	{
		double V = Default;
		if (O.IsValid()) O->TryGetNumberField(FStringView(Field), V);
		return V;
	}

	FString Str(const TSharedPtr<FJsonObject>& O, const TCHAR* Field)
	{
		FString V;
		if (O.IsValid()) O->TryGetStringField(FStringView(Field), V);
		return V;
	}

	/** A welcome's changed blocks, cell to kind. */
	TMap<FIntVector, FName> ReadWorld(const TSharedPtr<FJsonObject>& Frame)
	{
		TMap<FIntVector, FName> Cells;
		const TArray<TSharedPtr<FJsonValue>>* WorldJson;
		if (!Frame.IsValid() || !Frame->TryGetArrayField(TEXT("world"), WorldJson)) return Cells;
		Cells.Reserve(WorldJson->Num());
		for (const TSharedPtr<FJsonValue>& V : *WorldJson)
		{
			const TSharedPtr<FJsonObject> C = V.IsValid() ? V->AsObject() : nullptr;
			if (!C.IsValid()) continue;
			Cells.Add(FIntVector((int32)Num(C, TEXT("x")), (int32)Num(C, TEXT("y")), (int32)Num(C, TEXT("z"))), FName(*Str(C, TEXT("kind"))));
		}
		return Cells;
	}

	FCubePresenceRep ReadPresence(const TSharedPtr<FJsonObject>& P)
	{
		FCubePresenceRep R;
		R.Id = Str(P, TEXT("player_id")); R.Name = Str(P, TEXT("name")); R.Server = Str(P, TEXT("server")); R.Color = Str(P, TEXT("color"));
		R.X = Num(P, TEXT("x")); R.Y = Num(P, TEXT("y")); R.Z = Num(P, TEXT("z")); R.Yaw = Num(P, TEXT("yaw")); R.Pitch = Num(P, TEXT("pitch"));
		R.Health = Num(P, TEXT("health"), 20);
		R.bSneaking = Num(P, TEXT("sneaking")) == 1; R.bSprinting = Num(P, TEXT("sprinting")) == 1;
		return R;
	}

	FCubeBombRep ReadBomb(const TSharedPtr<FJsonObject>& Frame)
	{
		FCubeBombRep R;
		const TSharedPtr<FJsonObject>* B;
		if (!Frame.IsValid() || !Frame->TryGetObjectField(TEXT("bomb"), B)) return R;
		R.Id = Str(*B, TEXT("bomb_id")); R.State = Str(*B, TEXT("state")); R.Holder = Str(*B, TEXT("holder"));
		R.X = Num(*B, TEXT("x")); R.Y = Num(*B, TEXT("y")); R.Z = Num(*B, TEXT("z"));
		R.VX = Num(*B, TEXT("vx")); R.VY = Num(*B, TEXT("vy")); R.VZ = Num(*B, TEXT("vz"));
		R.AgeMs = (int32)Num(Frame, TEXT("age"));
		double Height;
		if (Frame->TryGetNumberField(TEXT("z"), Height)) { R.bHasHeight = true; R.Height = Height; }
		return R;
	}

	void ReadCube(const TSharedPtr<FJsonObject>& Cube, bool bDelete, TArray<FCubeChangeRep>& Out)
	{
		if (!Cube.IsValid()) return;
		FCubeChangeRep C;
		C.X = (int16)Num(Cube, TEXT("x")); C.Y = (int16)Num(Cube, TEXT("y")); C.Z = (int16)Num(Cube, TEXT("z"));
		// A deleted record restores the generated block: a dug-out grass block or a felled log comes back, which an
		// "air" override would keep dug out.
		C.Kind = bDelete ? CubeSpec::GeneratedIndex : CubeSpec::KindIndex(FName(*Str(Cube, TEXT("kind"))));
		C.On = Str(Cube, TEXT("placed_on"));
		Out.Add(C);
	}
}

bool UCubeWorldGameInstance::IsInNetworkedWorld() const
{
	const UWorld* W = GetWorld();
	return W && W->GetNetMode() == NM_Client;
}

void UCubeWorldGameInstance::CloseSockets()
{
	if (Socket.IsValid()) Socket->Close();
	if (PendingSocket.IsValid()) PendingSocket->Close();
	Socket.Reset();
	PendingSocket.Reset();
	SocketPlan = FSocketPlan();
}

void UCubeWorldGameInstance::Send(const TSharedRef<FJsonObject>& Frame)
{
	if (bViaSocket && Socket.IsValid() && Socket->IsConnected()) Socket->Send(ToText(Frame));
}

// A C# server is played from the client's own world. From an Unreal server's world the socket connects at once and the
// world becomes the client's own when the welcome comes (LeaveUnrealServerKeepWorld); on a fresh join, or without the
// client's engine (-noseamless), the client goes back to the local map first and connects once it is there.
void UCubeWorldGameInstance::ConnectSocket(const FString& RoomName, const FString& Host, int32 Port, bool bSecure, const FString& ReservationToken, bool bTeleport)
{
	// Make before break, from an Unreal server: the socket connects while the player still plays there, and the world is
	// kept when the C# server's welcome comes (LeaveUnrealServerKeepWorld). Without the client's engine, the old way: back
	// to the local map first, then connect.
	const bool bLeaveOnWelcome = IsInNetworkedWorld() && UCubeGameEngine::Get() && !bTeleport && bPlaced && !FParse::Param(FCommandLine::Get(), TEXT("noseamless"));
	if (IsInNetworkedWorld() && !bLeaveOnWelcome)
	{
		SocketPlan = { true, RoomName, Host, ReservationToken, Port, bSecure, bTeleport };
		Crossing = bTeleport ? FCubeCrossing() : LastBody;
		Crossing.bSet = !bTeleport && bPlaced;
		bWelcomed = false;
		Travelling = RoomName;
		if (!Crossing.bSet) Players.Empty();
		Log(FString::Printf(TEXT("leaving the Unreal server for %s (C#)"), *RoomName));
		if (APlayerController* PC = GetFirstLocalPlayerController()) PC->ClientTravel(TEXT("/Engine/Maps/Entry"), ETravelType::TRAVEL_Absolute);
		else bSwitching = false;
		return;
	}
	Log(FString::Printf(TEXT("connecting to %s://%s:%d"), bSecure ? TEXT("wss") : TEXT("ws"), *Host, Port));
	if (!bTeleport && bPlaced) LogFramesFor(3);
	TSharedPtr<FCubeSocket> NewSocket = MakeShared<FCubeSocket>(Host, Port, bSecure, TEXT("/"));
	PendingSocket = NewSocket;
	TWeakObjectPtr<UCubeWorldGameInstance> Weak(this);
	TWeakPtr<FCubeSocket> WeakSocket(NewSocket);
	const FString Token = UPlayServSubsystem::Get()->GetAuth()->GetAccessToken();

	NewSocket->OnConnected.AddLambda([Weak, WeakSocket, ReservationToken, Token]()
	{
		if (!Weak.IsValid() || !WeakSocket.IsValid()) return;
		const TSharedRef<FJsonObject> Hello = MakeShared<FJsonObject>();
		Hello->SetStringField(TEXT("playerId"), Weak->PlayerId);
		Hello->SetStringField(TEXT("displayName"), Weak->PlayerName);
		Hello->SetStringField(TEXT("token"), Token);
		Hello->SetStringField(TEXT("reservationToken"), ReservationToken);
		WeakSocket.Pin()->Send(ToText(Hello));
	});
	NewSocket->OnError.AddLambda([Weak, WeakSocket, RoomName](const FString& Error)
	{
		if (!Weak.IsValid()) return;
		const bool bLive = Weak->Socket == WeakSocket.Pin();
		Weak->bSwitching = false;
		Weak->CrossAfter = FPlatformTime::Seconds() + 3;
		Weak->Log(FString::Printf(TEXT("could not reach %s: %s"), *RoomName, *Error));
		if (bLive) { Weak->Socket.Reset(); Weak->Room.Empty(); Weak->bViaSocket = false; Weak->Disconnected(Error); }
		else if (!Weak->IsConnected()) { Weak->Travelling.Empty(); Weak->Reconnect(); }
	});
	NewSocket->OnClosed.AddLambda([Weak, WeakSocket, RoomName](const FString& Reason)
	{
		if (!Weak.IsValid()) return;
		if (Weak->Socket == WeakSocket.Pin())
		{
			Weak->Socket.Reset();
			Weak->bViaSocket = false;
			Weak->Disconnected(Reason);
		}
		else Weak->bSwitching = false;
	});
	NewSocket->Decode = &UCubeWorldGameInstance::DecodeSocketFrame;
	NewSocket->OnFrame.AddLambda([Weak, WeakSocket, RoomName, bTeleport](FCubeSocketFrame& Parsed)
	{
		if (!Weak.IsValid() || !WeakSocket.IsValid()) return;
		const TSharedPtr<FJsonObject> Frame = Parsed.Json;
		const TSharedPtr<FCubeSocket> This = WeakSocket.Pin();
		if (Str(Frame, TEXT("type")) == TEXT("welcome") && Weak->Socket != This)
		{
			// Still on an Unreal server: it is left now, and the world with everything on screen stays.
			if (Weak->IsInNetworkedWorld()) Weak->LeaveUnrealServerKeepWorld();
			const TSharedPtr<FCubeSocket> Previous = Weak->Socket;
			Weak->Socket = This;
			Weak->PendingSocket.Reset();
			if (Previous.IsValid()) Previous->Close();
			Weak->OnSocketWelcome(Frame, RoomName, bTeleport || !Weak->bPlaced, MoveTemp(Parsed.World));
			return;
		}
		if (Weak->Socket == This) Weak->OnSocketFrame(Frame, RoomName, false);
	});
	NewSocket->Connect();
}

// A C# server's welcome carries every changed block of the world: the worker that parses it reads the blocks too, so the
// game thread only swaps them in, and drops them from the frame, so that their JSON is freed there and not on the game
// thread, where freeing it took most of a crossing's frame (PSV-3004).
void UCubeWorldGameInstance::DecodeSocketFrame(FCubeSocketFrame& Parsed)
{
	if (!Parsed.Json.IsValid() || !Parsed.Json->HasField(TEXT("world"))) return;
	Parsed.World = ReadWorld(Parsed.Json);
	Parsed.Json->RemoveField(TEXT("world"));
}

void UCubeWorldGameInstance::OnSocketWelcome(const TSharedPtr<FJsonObject>& Frame, const FString& RoomName, bool bTeleport, TOptional<TMap<FIntVector, FName>> PreRead)
{
	const bool bCrossed = !bTeleport;
	bViaSocket = true;
	Server = Str(Frame, TEXT("server")); Color = Str(Frame, TEXT("color")); Region = (int32)Num(Frame, TEXT("region"), -1);
	Room = RoomName;
	Travelling.Empty();
	bSwitching = false;
	bSigningIn = false;
	bWelcomed = true;
	bDead = false;
	Status.Empty();
	OnSocketFrame(Frame, RoomName, bTeleport);   // the regions
	// A fresh join starts from the generated terrain; a crossing keeps the world on screen and applies only what differs.
	const bool bKeepWorld = bCrossed && bPlaced;
	Snapshot.Reset();
	if (!bKeepWorld) World.Clear();
	// Read on the worker that parsed the frame; a welcome handed in without it is read here.
	const double ApplyStart = FPlatformTime::Seconds();
	TMap<FIntVector, FName> Cells = PreRead.IsSet() ? MoveTemp(PreRead.GetValue()) : ReadWorld(Frame);
	const int32 Blocks = Cells.Num();
	if (bKeepWorld) { Snapshot = MoveTemp(Cells); ApplySnapshot(); }
	else for (const TPair<FIntVector, FName>& C : Cells) World.Set(C.Key.X, C.Key.Y, C.Key.Z, C.Value);
	Log(FString::Printf(TEXT("welcome: %d blocks applied in %.1f ms"), Blocks, (FPlatformTime::Seconds() - ApplyStart) * 1000));
	const TSharedPtr<FJsonObject>* InventoryJson;
	if (Frame->TryGetObjectField(TEXT("inventory"), InventoryJson))
	{
		TArray<FCubeStackRep> Stacks;
		for (const auto& Pair : (*InventoryJson)->Values) Stacks.Add({ CubeSpec::KindIndex(FName(*Pair.Key)), (int32)Pair.Value->AsNumber() });
		SetInventory(Stacks);
	}
	const TSharedPtr<FJsonObject>* You;
	FCubePose Pose;
	if (Frame->TryGetObjectField(TEXT("you"), You)) { Pose.X = Num(*You, TEXT("x")); Pose.Y = Num(*You, TEXT("y")); Pose.Z = Num(*You, TEXT("z")); Pose.Health = Num(*You, TEXT("health"), 20); }
	Health = Pose.Health;
	// The C# server spawns a crossing player at its spawn and takes their moves from there; the body keeps its place.
	WelcomePose = bCrossed && Crossing.bSet ? FCubePose{ Crossing.X, Crossing.Y, Crossing.Z, Pose.Health } : Pose;
	Crossing = FCubeCrossing();
	bWorldLoaded = true;
	Log(FString::Printf(TEXT("%s %s (C#)"), bCrossed ? TEXT("crossed into") : TEXT("entered"), *RoomName));
	OnWelcome.Broadcast(WelcomePose, !bKeepWorld);
	bPlaced = true;
	const TArray<TSharedPtr<FJsonValue>>* BombsJson;
	if (Frame->TryGetArrayField(TEXT("bombs"), BombsJson))
		for (const auto& V : *BombsJson) OnBombFrame(ReadBomb(V->AsObject()));
	// This client shows bombs and reads batched cube frames; the server hands both only to clients that say so.
	const TSharedRef<FJsonObject> Bombs = MakeShared<FJsonObject>();
	Bombs->SetStringField(TEXT("op"), TEXT("bombs"));
	Send(Bombs);
}

void UCubeWorldGameInstance::OnSocketFrame(const TSharedPtr<FJsonObject>& Frame, const FString& RoomName, bool)
{
	const FString Type = Str(Frame, TEXT("type"));
	if (Type == TEXT("welcome") || Type == TEXT("regions"))
	{
		TArray<FCubeRegionRep> List;
		const TArray<TSharedPtr<FJsonValue>>* RegionsJson;
		if (Frame->TryGetArrayField(TEXT("regions"), RegionsJson))
			for (const auto& V : *RegionsJson)
			{
				const TSharedPtr<FJsonObject> R = V->AsObject();
				List.Add({ FCString::Atoi(*Str(R, TEXT("region"))), Str(R, TEXT("room")), Str(R, TEXT("color")), Str(R, TEXT("server")), Str(R, TEXT("slug")) });
			}
		SetRegions(List);
		return;
	}
	if (Type == TEXT("cube"))
	{
		const TSharedPtr<FJsonObject>* C;
		if (!Frame->TryGetObjectField(TEXT("cube"), C)) return;
		TArray<FCubeChangeRep> Changes;
		ReadCube(*C, Str(Frame, TEXT("op")) == TEXT("delete"), Changes);
		bool bRemote = false;
		Frame->TryGetBoolField(TEXT("remote"), bRemote);
		ApplyCubes(Changes, TArray<FCubeFallRep>(), bRemote);
		return;
	}
	if (Type == TEXT("fall"))
	{
		OnFall.Broadcast(FName(*Str(Frame, TEXT("kind"))), (int32)Num(Frame, TEXT("x")), (int32)Num(Frame, TEXT("y")), (int32)Num(Frame, TEXT("fromZ")), (int32)Num(Frame, TEXT("toZ")));
		return;
	}
	if (Type == TEXT("cubes"))
	{
		TArray<FCubeFallRep> Falls;
		const TArray<TSharedPtr<FJsonValue>>* FallsJson;
		if (Frame->TryGetArrayField(TEXT("falls"), FallsJson))
			for (const auto& V : *FallsJson)
			{
				const TSharedPtr<FJsonObject> F = V->AsObject();
				Falls.Add({ CubeSpec::KindIndex(FName(*Str(F, TEXT("kind")))), (int16)Num(F, TEXT("x")), (int16)Num(F, TEXT("y")), (int16)Num(F, TEXT("fromZ")), (int16)Num(F, TEXT("toZ")) });
			}
		TArray<FCubeChangeRep> Changes;
		const TArray<TSharedPtr<FJsonValue>>* ChangesJson;
		if (Frame->TryGetArrayField(TEXT("changes"), ChangesJson))
			for (const auto& V : *ChangesJson)
			{
				const TSharedPtr<FJsonObject> Change = V->AsObject();
				const TSharedPtr<FJsonObject>* C;
				if (Change.IsValid() && Change->TryGetObjectField(TEXT("cube"), C)) ReadCube(*C, Str(Change, TEXT("op")) == TEXT("delete"), Changes);
			}
		bool bRemote = false;
		Frame->TryGetBoolField(TEXT("remote"), bRemote);
		ApplyCubes(Changes, Falls, bRemote);
		return;
	}
	if (Type == TEXT("bomb")) { OnBombFrame(ReadBomb(Frame)); return; }
	if (Type == TEXT("dig"))
	{
		OnDig.Broadcast(Str(Frame, TEXT("player")), (int32)Num(Frame, TEXT("x")), (int32)Num(Frame, TEXT("y")), (int32)Num(Frame, TEXT("z")), (int32)Num(Frame, TEXT("stage"), -1));
		return;
	}
	if (Type == TEXT("inventory") || Type == TEXT("refused"))
	{
		const TSharedPtr<FJsonObject>* InventoryJson;
		if (!Frame->TryGetObjectField(TEXT("inventory"), InventoryJson)) return;
		TArray<FCubeStackRep> Stacks;
		for (const auto& Pair : (*InventoryJson)->Values) Stacks.Add({ CubeSpec::KindIndex(FName(*Pair.Key)), (int32)Pair.Value->AsNumber() });
		SetInventory(Stacks);
		return;
	}
	if (Type == TEXT("players"))
	{
		TArray<FCubePresenceRep> List;
		const TArray<TSharedPtr<FJsonValue>>* PlayersJson;
		if (Frame->TryGetArrayField(TEXT("players"), PlayersJson))
			for (const auto& V : *PlayersJson) List.Add(ReadPresence(V->AsObject()));
		SetPlayers(List);
		return;
	}
	if (Type == TEXT("hurt")) { OnHurtFrame(Str(Frame, TEXT("player")), Num(Frame, TEXT("health")), Num(Frame, TEXT("kx")), Num(Frame, TEXT("ky")), Num(Frame, TEXT("strength"))); return; }
	if (Type == TEXT("death")) { OnDeathFrame(Str(Frame, TEXT("player")), Str(Frame, TEXT("by"))); return; }
	if (Type == TEXT("respawn"))
	{
		const TSharedPtr<FJsonObject>* You;
		FCubePose Pose;
		if (Frame->TryGetObjectField(TEXT("you"), You)) { Pose.X = Num(*You, TEXT("x")); Pose.Y = Num(*You, TEXT("y")); Pose.Z = Num(*You, TEXT("z")); Pose.Health = Num(*You, TEXT("health"), 20); }
		OnRespawnFrame(Pose);
	}
}
