#include "CubeWorldGameInstance.h"
#include "CubeWorld.h"
#include "CubeSocket.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "PlayServ.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

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

	FCubePresence ReadPresence(const TSharedPtr<FJsonObject>& P)
	{
		FCubePresence R;
		R.Id = Str(P, TEXT("player_id")); R.Name = Str(P, TEXT("name")); R.Server = Str(P, TEXT("server")); R.Color = Str(P, TEXT("color"));
		R.X = Num(P, TEXT("x")); R.Y = Num(P, TEXT("y")); R.Z = Num(P, TEXT("z")); R.Yaw = Num(P, TEXT("yaw")); R.Pitch = Num(P, TEXT("pitch"));
		R.Health = Num(P, TEXT("health"), 20);
		R.bSneaking = Num(P, TEXT("sneaking")) == 1; R.bSprinting = Num(P, TEXT("sprinting")) == 1;
		return R;
	}

	FCubeBombFrame ReadBomb(const TSharedPtr<FJsonObject>& Frame)
	{
		FCubeBombFrame R;
		const TSharedPtr<FJsonObject>* B;
		if (!Frame.IsValid() || !Frame->TryGetObjectField(TEXT("bomb"), B)) return R;
		R.Id = Str(*B, TEXT("bomb_id")); R.State = Str(*B, TEXT("state")); R.Holder = Str(*B, TEXT("holder"));
		R.X = Num(*B, TEXT("x")); R.Y = Num(*B, TEXT("y")); R.Z = Num(*B, TEXT("z"));
		R.VX = Num(*B, TEXT("vx")); R.VY = Num(*B, TEXT("vy")); R.VZ = Num(*B, TEXT("vz"));
		R.Age = Num(Frame, TEXT("age"));
		double Height;
		if (Frame->TryGetNumberField(TEXT("z"), Height)) R.Height = Height;
		return R;
	}

	FCubePose ReadPose(const TSharedPtr<FJsonObject>& P)
	{
		FCubePose R;
		R.X = Num(P, TEXT("x")); R.Y = Num(P, TEXT("y")); R.Z = Num(P, TEXT("z")); R.Health = Num(P, TEXT("health"), 20);
		return R;
	}
}

void UCubeWorldGameInstance::Init()
{
	Super::Init();
	Textures.Build();
}

void UCubeWorldGameInstance::Shutdown()
{
	if (Socket.IsValid()) Socket->Close();
	if (Pending.IsValid()) Pending->Close();
	Socket.Reset();
	Pending.Reset();
	Super::Shutdown();
}

bool UCubeWorldGameInstance::IsConnected() const
{
	return Socket.IsValid() && Socket->IsConnected();
}

void UCubeWorldGameInstance::Log(const FString& Text)
{
	UE_LOG(LogCubeWorld, Log, TEXT("%s"), *Text);
	LogLines.Insert(Text, 0);
	if (LogLines.Num() > 8) LogLines.SetNum(8);
}

void UCubeWorldGameInstance::StartPlay(const FString& Name)
{
	if (bSigningIn || IsConnected()) return;
	PlayerName = Name;
	bSigningIn = true;
	Status = TEXT("Signing in...");
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
	if (ReasonOrCode == TEXT("room_closed_by_operator") || ReasonOrCode == TEXT("room_closed"))
	{
		Wait = 30;
		Message = TEXT("This room was closed by an operator. It opens again fresh in a minute or two.");
	}
	else if (ReasonOrCode == TEXT("removed_by_operator") || ReasonOrCode == TEXT("removed_from_room"))
	{
		Wait = 60;
		Message = TEXT("An operator removed you from this room. You can still walk into the other regions.");
	}
	if (!Message.IsEmpty()) NotBefore.Add(RoomName, FPlatformTime::Seconds() + Wait);
	return Message;
}

void UCubeWorldGameInstance::Browse()
{
	Status = TEXT("Looking for servers...");
	TWeakObjectPtr<UCubeWorldGameInstance> Weak(this);
	PlayServ::Rooms::Browse(FPlayServRoomFilters(), FPlayServBrowseCallback::CreateLambda([Weak](bool bOk, const FPlayServBrowsePage& Page, const FPlayServError& Error)
	{
		if (!Weak.IsValid()) return;
		UCubeWorldGameInstance* Self = Weak.Get();
		if (!bOk || Page.Rooms.Num() == 0)
		{
			Self->bSigningIn = false;
			Self->Status = bOk ? TEXT("No server is running. Press Enter to retry.") : FString::Printf(TEXT("Browse failed: %s"), *Error.Message);
			Self->Log(Self->Status);
			return;
		}
		Self->Candidates.Empty();
		for (const FPlayServRoomListing& R : Page.Rooms)
			if ((R.PlacementState == EPlayServPlacementState::Open || R.PlacementState == EPlayServPlacementState::Unknown) && Self->MayTry(R.RoomName)) Self->Candidates.Add(R.RoomName);
		Self->Candidates.Sort();
		if (Self->Candidates.Num() == 0)
		{
			Self->Status = TEXT("Every server is closing or full, retrying...");
			Self->GetTimerManager().SetTimer(Self->RetryTimer, [Weak]() { if (Weak.IsValid()) Weak->Browse(); }, 3.f, false);
			return;
		}
		const FString First = Self->Candidates[0];
		Self->Candidates.RemoveAt(0);
		Self->Enter(First, true);
	}));
}

void UCubeWorldGameInstance::Enter(const FString& RoomName, bool bTeleport)
{
	if (RoomName == Room || bSwitching) return;
	bSwitching = true;
	Status = FString::Printf(TEXT("Joining %s..."), *RoomName);
	TWeakObjectPtr<UCubeWorldGameInstance> Weak(this);
	PlayServ::Rooms::JoinRoom(RoomName, nullptr, FPlayServJoinCallback::CreateLambda([Weak, RoomName, bTeleport](bool bOk, const FPlayServJoinResult& Result, const FPlayServError& Error)
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
				if (Self->Candidates.Num() > 0) { const FString Next = Self->Candidates[0]; Self->Candidates.RemoveAt(0); Self->Enter(Next, bTeleport); }
				else Self->GetTimerManager().SetTimer(Self->RetryTimer, [Weak]() { if (Weak.IsValid()) Weak->Browse(); }, 3.f, false);
			}
			return;
		}
		const FPlayServRoomConnect& C = Result.Ticket.Connect;
		FString Host = C.Host, Path = TEXT("/"), Override;
		int32 Port = C.Port;
		// -wshost=127.0.0.1:7777 points the game socket elsewhere, for tests.
		if (FParse::Value(FCommandLine::Get(), TEXT("-wshost="), Override) && !Override.IsEmpty())
		{
			FString PortText;
			if (Override.Split(TEXT(":"), &Host, &PortText)) Port = FCString::Atoi(*PortText); else Host = Override;
		}
		Self->Connect(RoomName, Host, Port, Path, Result.Ticket.ReservationToken, bTeleport);
	}));
}

void UCubeWorldGameInstance::Connect(const FString& RoomName, const FString& Host, int32 Port, const FString& Path, const FString& ReservationToken, bool bTeleport)
{
	Log(FString::Printf(TEXT("connecting to %s:%d"), *Host, Port));
	TSharedPtr<FCubeSocket> NewSocket = MakeShared<FCubeSocket>(Host, Port, Path);
	Pending = NewSocket;
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
		Weak->bSigningIn = false;
		Weak->Status = FString::Printf(TEXT("Could not reach %s: %s"), *RoomName, *Error);
		Weak->Log(Weak->Status);
		if (bLive) { Weak->Socket.Reset(); Weak->Room.Empty(); Weak->Reconnect(); }
	});
	NewSocket->OnClosed.AddLambda([Weak, WeakSocket, RoomName](const FString& Reason)
	{
		if (!Weak.IsValid()) return;
		if (Weak->Socket == WeakSocket.Pin())
		{
			const FString Turned = Weak->TurnedAway(RoomName, Reason);
			Weak->Log(Turned.IsEmpty() ? FString::Printf(TEXT("disconnected: %s"), *Reason) : Turned);
			Weak->Socket.Reset();
			Weak->Room.Empty();
			Weak->bSigningIn = false;
			Weak->Reconnect();
		}
		else Weak->bSwitching = false;
	});
	NewSocket->OnMessage.AddLambda([Weak, WeakSocket, RoomName, bTeleport](const FString& Text)
	{
		if (!Weak.IsValid() || !WeakSocket.IsValid()) return;
		UCubeWorldGameInstance* Self = Weak.Get();
		TSharedPtr<FJsonObject> Frame;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Frame) || !Frame.IsValid()) return;
		const FString Type = Str(Frame, TEXT("type"));
		const TSharedPtr<FCubeSocket> This = WeakSocket.Pin();
		if (Type == TEXT("welcome") && Self->Socket != This)
		{
			const TSharedPtr<FCubeSocket> Previous = Self->Socket;
			Self->Socket = This;
			Self->Pending.Reset();
			Self->Room = RoomName;
			Self->bSwitching = false;
			Self->bSigningIn = false;
			if (Previous.IsValid()) Previous->Close();
			Self->OnFrame(Frame, bTeleport || !Self->bPlaced);
			Self->Log(FString::Printf(TEXT("%s %s"), Previous.IsValid() ? TEXT("crossed into") : TEXT("entered"), *RoomName));
			return;
		}
		if (Self->Socket == This) Self->OnFrame(Frame, false);
	});
	NewSocket->Connect();
}

// After the live socket goes: look for a server again in a moment, keeping the sign-in.
void UCubeWorldGameInstance::Reconnect()
{
	Status = TEXT("Disconnected, reconnecting...");
	TWeakObjectPtr<UCubeWorldGameInstance> Weak(this);
	GetTimerManager().SetTimer(RetryTimer, [Weak]() { if (Weak.IsValid() && !Weak->IsConnected() && !Weak->PlayerId.IsEmpty()) { Weak->bSigningIn = true; Weak->Browse(); } }, 3.f, false);
}

void UCubeWorldGameInstance::Send(const TSharedRef<FJsonObject>& Frame)
{
	if (IsConnected()) Socket->Send(ToText(Frame));
}

FString UCubeWorldGameInstance::RoomOfRegion(int32 InRegion) const
{
	for (const FCubeRegion& R : Regions) if (R.Region == InRegion) return R.Room;
	return FString();
}

void UCubeWorldGameInstance::MaybeCross(double X)
{
	if (!bPlaced || bSwitching || !IsConnected() || FPlatformTime::Seconds() < CrossAfter) return;
	const FString Here = RoomOfRegion(FMath::FloorToInt32(X / World.RegionSize));
	if (!Here.IsEmpty() && Here != Room && MayTry(Here)) Enter(Here, false);
}

FString UCubeWorldGameInstance::NameOf(const FString& Id) const
{
	if (Id == PlayerId) return TEXT("you");
	for (const FCubePresence& P : Players) if (P.Id == Id) return P.Name;
	return Id;
}

void UCubeWorldGameInstance::ReadCube(const TSharedPtr<FJsonObject>& Cube, bool bDelete, TArray<FIntVector>& Changed)
{
	if (!Cube.IsValid()) return;
	const FName Kind = bDelete ? NAME_None : FName(*Str(Cube, TEXT("kind")));
	const int32 X = (int32)Num(Cube, TEXT("x")), Y = (int32)Num(Cube, TEXT("y")), Z = (int32)Num(Cube, TEXT("z"));
	World.Set(X, Y, Z, Kind);
	Changed.Add(FIntVector(X, Y, Z));
	OnCube.Broadcast(X, Y, Z, Kind);
}

void UCubeWorldGameInstance::ReadFall(const TSharedPtr<FJsonObject>& Fall)
{
	OnFall.Broadcast(FName(*Str(Fall, TEXT("kind"))), (int32)Num(Fall, TEXT("x")), (int32)Num(Fall, TEXT("y")), (int32)Num(Fall, TEXT("fromZ")), (int32)Num(Fall, TEXT("toZ")));
}

void UCubeWorldGameInstance::ReadInventory(const TSharedPtr<FJsonObject>& Object)
{
	if (!Object.IsValid()) return;
	Inventory.Empty();
	for (const auto& Pair : Object->Values) Inventory.Add(FName(*Pair.Key), (int32)Pair.Value->AsNumber());
	OnInventory.Broadcast();
}

void UCubeWorldGameInstance::OnFrame(const TSharedPtr<FJsonObject>& Frame, bool bTeleport)
{
	const FString Type = Str(Frame, TEXT("type"));
	if (Type != TEXT("players") && FParse::Param(FCommandLine::Get(), TEXT("logframes"))) UE_LOG(LogCubeWorld, Log, TEXT("frame %s"), *Type);
	if (Type == TEXT("welcome"))
	{
		Server = Str(Frame, TEXT("server")); Color = Str(Frame, TEXT("color")); Region = (int32)Num(Frame, TEXT("region"), -1);
		Regions.Empty();
		const TArray<TSharedPtr<FJsonValue>>* RegionsJson;
		if (Frame->TryGetArrayField(TEXT("regions"), RegionsJson))
			for (const auto& V : *RegionsJson)
			{
				const TSharedPtr<FJsonObject> R = V->AsObject();
				FCubeRegion Reg; Reg.Region = FCString::Atoi(*Str(R, TEXT("region"))); Reg.Room = Str(R, TEXT("room")); Reg.Color = Str(R, TEXT("color")); Reg.Server = Str(R, TEXT("server"));
				Regions.Add(Reg);
			}
		TMap<int32, FName> Layers;
		const TArray<TSharedPtr<FJsonValue>>* LayersJson;
		if (Frame->TryGetArrayField(TEXT("layers"), LayersJson))
			for (const auto& V : *LayersJson) Layers.Add((int32)Num(V->AsObject(), TEXT("z")), FName(*Str(V->AsObject(), TEXT("kind"))));
		TArray<FIntPoint> Trees;
		const TArray<TSharedPtr<FJsonValue>>* TreesJson;
		if (Frame->TryGetArrayField(TEXT("trees"), TreesJson))
			for (const auto& V : *TreesJson) Trees.Add(FIntPoint((int32)Num(V->AsObject(), TEXT("x")), (int32)Num(V->AsObject(), TEXT("y"))));
		TArray<FBlockDef> Blocks;
		const TArray<TSharedPtr<FJsonValue>>* BlocksJson;
		if (Frame->TryGetArrayField(TEXT("blocks"), BlocksJson))
			for (const auto& V : *BlocksJson)
			{
				const TSharedPtr<FJsonObject> B = V->AsObject();
				FBlockDef Def;
				Def.Kind = FName(*Str(B, TEXT("kind")));
				Def.Hardness = Num(B, TEXT("Hardness"));
				Def.bNeedsTool = B->HasTypedField<EJson::Boolean>(TEXT("NeedsTool")) && B->GetBoolField(TEXT("NeedsTool"));
				Def.bTransparent = B->HasTypedField<EJson::Boolean>(TEXT("Transparent")) && B->GetBoolField(TEXT("Transparent"));
				Def.bGravity = B->HasTypedField<EJson::Boolean>(TEXT("Gravity")) && B->GetBoolField(TEXT("Gravity"));
				const FString Drop = Str(B, TEXT("Drop"));
				Def.Drop = Drop.IsEmpty() ? NAME_None : FName(*Drop);
				Def.BreakTicks = (int32)Num(B, TEXT("breakTicks"), -1);
				Blocks.Add(Def);
			}
		World.Configure((int32)Num(Frame, TEXT("width"), 72), (int32)Num(Frame, TEXT("depth"), 24), (int32)Num(Frame, TEXT("minZ"), -4), (int32)Num(Frame, TEXT("maxZ"), 64),
			(int32)Num(Frame, TEXT("regionSize"), 24), Layers, Trees, Blocks);
		const TArray<TSharedPtr<FJsonValue>>* WorldJson;
		if (Frame->TryGetArrayField(TEXT("world"), WorldJson))
			for (const auto& V : *WorldJson)
			{
				const TSharedPtr<FJsonObject> C = V->AsObject();
				World.Set((int32)Num(C, TEXT("x")), (int32)Num(C, TEXT("y")), (int32)Num(C, TEXT("z")), FName(*Str(C, TEXT("kind"))));
			}
		Hotbar.Empty();
		const TArray<TSharedPtr<FJsonValue>>* HotbarJson;
		if (Frame->TryGetArrayField(TEXT("hotbar"), HotbarJson)) for (const auto& V : *HotbarJson) Hotbar.Add(FName(*V->AsString()));
		const TSharedPtr<FJsonObject>* InventoryJson;
		if (Frame->TryGetObjectField(TEXT("inventory"), InventoryJson)) ReadInventory(*InventoryJson);
		const TSharedPtr<FJsonObject>* You;
		FCubePose Pose;
		if (Frame->TryGetObjectField(TEXT("you"), You)) Pose = ReadPose(*You);
		Health = Pose.Health;
		bDead = false;
		Status.Empty();
		OnWelcome.Broadcast(Pose, bTeleport);
		if (bTeleport) bPlaced = true;
		const TArray<TSharedPtr<FJsonValue>>* BombsJson;
		if (Frame->TryGetArrayField(TEXT("bombs"), BombsJson))
			for (const auto& V : *BombsJson) OnBomb.Broadcast(ReadBomb(V->AsObject()));
		// This client can show a bomb in the hand and throw it, and reads blocks batched in one "cubes" frame; the
		// server hands bombs, and batches, only to clients that say so.
		const TSharedRef<FJsonObject> Bombs = MakeShared<FJsonObject>();
		Bombs->SetStringField(TEXT("op"), TEXT("bombs"));
		Send(Bombs);
		return;
	}
	if (Type == TEXT("regions"))
	{
		Regions.Empty();
		const TArray<TSharedPtr<FJsonValue>>* RegionsJson;
		if (Frame->TryGetArrayField(TEXT("regions"), RegionsJson))
			for (const auto& V : *RegionsJson)
			{
				const TSharedPtr<FJsonObject> R = V->AsObject();
				FCubeRegion Reg; Reg.Region = FCString::Atoi(*Str(R, TEXT("region"))); Reg.Room = Str(R, TEXT("room")); Reg.Color = Str(R, TEXT("color")); Reg.Server = Str(R, TEXT("server"));
				Regions.Add(Reg);
			}
		return;
	}
	if (Type == TEXT("cube"))
	{
		const TSharedPtr<FJsonObject>* C;
		if (!Frame->TryGetObjectField(TEXT("cube"), C)) return;
		const bool bDelete = Str(Frame, TEXT("op")) == TEXT("delete");
		const FName Kind = bDelete ? NAME_None : FName(*Str(*C, TEXT("kind")));
		TArray<FIntVector> Changed;
		ReadCube(*C, bDelete, Changed);
		OnCubes.Broadcast(Changed);
		if (Frame->HasTypedField<EJson::Boolean>(TEXT("remote")) && Frame->GetBoolField(TEXT("remote")))
			Log(FString::Printf(TEXT("%s on server %s -> arrived here"), Kind == NAME_None || Kind == TEXT("air") ? TEXT("removed") : TEXT("placed"), *Str(*C, TEXT("placed_on"))));
		return;
	}
	if (Type == TEXT("fall"))
	{
		ReadFall(Frame);
		return;
	}
	if (Type == TEXT("cubes"))
	{
		// Blocks that changed together (a blast is a hundred of them) arrive in one frame and rebuild each chunk once.
		const TArray<TSharedPtr<FJsonValue>>* Falls;
		if (Frame->TryGetArrayField(TEXT("falls"), Falls)) for (const auto& V : *Falls) ReadFall(V->AsObject());
		TArray<FIntVector> Changed;
		FString PlacedOn;
		const TArray<TSharedPtr<FJsonValue>>* Changes;
		if (Frame->TryGetArrayField(TEXT("changes"), Changes))
			for (const auto& V : *Changes)
			{
				const TSharedPtr<FJsonObject> Change = V->AsObject();
				const TSharedPtr<FJsonObject>* C;
				if (!Change.IsValid() || !Change->TryGetObjectField(TEXT("cube"), C)) continue;
				ReadCube(*C, Str(Change, TEXT("op")) == TEXT("delete"), Changed);
				if (PlacedOn.IsEmpty()) PlacedOn = Str(*C, TEXT("placed_on"));
			}
		OnCubes.Broadcast(Changed);
		if (Changed.Num() > 0 && Frame->HasTypedField<EJson::Boolean>(TEXT("remote")) && Frame->GetBoolField(TEXT("remote")))
			Log(FString::Printf(TEXT("%d block%s changed on server %s -> arrived here"), Changed.Num(), Changed.Num() > 1 ? TEXT("s") : TEXT(""), *PlacedOn));
		return;
	}
	if (Type == TEXT("bomb"))
	{
		OnBomb.Broadcast(ReadBomb(Frame));
		return;
	}
	if (Type == TEXT("dig"))
	{
		OnDig.Broadcast(Str(Frame, TEXT("player")), (int32)Num(Frame, TEXT("x")), (int32)Num(Frame, TEXT("y")), (int32)Num(Frame, TEXT("z")), (int32)Num(Frame, TEXT("stage"), -1));
		return;
	}
	if (Type == TEXT("inventory") || Type == TEXT("refused"))
	{
		const TSharedPtr<FJsonObject>* InventoryJson;
		if (Frame->TryGetObjectField(TEXT("inventory"), InventoryJson)) ReadInventory(*InventoryJson);
		return;
	}
	if (Type == TEXT("players"))
	{
		Players.Empty();
		const TArray<TSharedPtr<FJsonValue>>* PlayersJson;
		if (Frame->TryGetArrayField(TEXT("players"), PlayersJson))
			for (const auto& V : *PlayersJson) Players.Add(ReadPresence(V->AsObject()));
		OnPlayers.Broadcast(Players);
		return;
	}
	if (Type == TEXT("hurt"))
	{
		const FString Who = Str(Frame, TEXT("player"));
		if (Who == PlayerId) Health = Num(Frame, TEXT("health"));
		OnHurt.Broadcast(Who, Num(Frame, TEXT("health")), Num(Frame, TEXT("kx")), Num(Frame, TEXT("ky")), Num(Frame, TEXT("strength")));
		return;
	}
	if (Type == TEXT("death"))
	{
		const FString Who = Str(Frame, TEXT("player"));
		if (Who == PlayerId) { bDead = true; Health = 0; }
		OnDeath.Broadcast(Who, Str(Frame, TEXT("by")));
		return;
	}
	if (Type == TEXT("respawn"))
	{
		const TSharedPtr<FJsonObject>* You;
		FCubePose Pose;
		if (Frame->TryGetObjectField(TEXT("you"), You)) Pose = ReadPose(*You);
		bDead = false;
		Health = Pose.Health;
		OnRespawn.Broadcast(Pose);
	}
}
