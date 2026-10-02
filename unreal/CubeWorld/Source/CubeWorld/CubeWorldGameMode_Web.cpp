// The WebSocket door: a browser client (or an Unreal client that prefers it) plays on this server through the
// JSON frames the C# server speaks (CubeWorld.Server/CubeWorldServer.cs, web/app.js), so a player in a browser
// and a player on Unreal's netcode share the room. The handshake carries the platform's ticket, which admits the
// player as PreLogin does for an Unreal client; from then on their commands and frames are the same game.
#include "CubeWorldGameMode.h"
#include "CubeWorld.h"
#include "CubePlayerPawn.h"
#include "CubeWebSocketServer.h"
#include "CubeEntities.h"
#include "PlayServ.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Policies/CondensedJsonPrintPolicy.h"
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

	bool Flag(const TSharedPtr<FJsonObject>& O, const TCHAR* Field)
	{
		bool V = false;
		if (O.IsValid()) O->TryGetBoolField(FStringView(Field), V);
		return V;
	}

	TSharedRef<FJsonObject> Frame(const TCHAR* Type)
	{
		TSharedRef<FJsonObject> F = MakeShared<FJsonObject>();
		F->SetStringField(TEXT("type"), Type);
		return F;
	}

	TSharedRef<FJsonObject> PoseJson(const FCubePresenceRep& P, int64 SeenAt)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("player_id"), P.Id); J->SetStringField(TEXT("name"), P.Name);
		J->SetStringField(TEXT("server"), P.Server); J->SetStringField(TEXT("color"), P.Color);
		J->SetNumberField(TEXT("x"), P.X); J->SetNumberField(TEXT("y"), P.Y); J->SetNumberField(TEXT("z"), P.Z);
		J->SetNumberField(TEXT("yaw"), P.Yaw); J->SetNumberField(TEXT("pitch"), P.Pitch); J->SetNumberField(TEXT("health"), P.Health);
		J->SetNumberField(TEXT("sneaking"), P.bSneaking ? 1 : 0); J->SetNumberField(TEXT("sprinting"), P.bSprinting ? 1 : 0);
		J->SetNumberField(TEXT("seen_at"), (double)SeenAt);
		return J;
	}

	TSharedRef<FJsonObject> BombJson(const FCubeBombRep& B)
	{
		TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetStringField(TEXT("bomb_id"), B.Id); R->SetStringField(TEXT("state"), B.State); R->SetStringField(TEXT("holder"), B.Holder);
		R->SetNumberField(TEXT("x"), B.X); R->SetNumberField(TEXT("y"), B.Y); R->SetNumberField(TEXT("z"), B.Z);
		R->SetNumberField(TEXT("vx"), B.VX); R->SetNumberField(TEXT("vy"), B.VY); R->SetNumberField(TEXT("vz"), B.VZ);
		TSharedRef<FJsonObject> F = Frame(TEXT("bomb"));
		F->SetObjectField(TEXT("bomb"), R);
		F->SetNumberField(TEXT("age"), B.AgeMs);
		F->SetNumberField(TEXT("z"), B.bHasHeight ? B.Height : B.Z);
		return F;
	}

	TSharedRef<FJsonObject> CubeJson(const FIntVector& At, FName Kind, const FString& By, const FString& On)
	{
		TSharedRef<FJsonObject> C = MakeShared<FJsonObject>();
		C->SetStringField(TEXT("key"), FCubeServerWorld::Key(At.X, At.Y, At.Z));
		C->SetNumberField(TEXT("x"), At.X); C->SetNumberField(TEXT("y"), At.Y); C->SetNumberField(TEXT("z"), At.Z);
		C->SetStringField(TEXT("kind"), CubeSpec::KindName(Kind)); C->SetStringField(TEXT("placed_by"), By); C->SetStringField(TEXT("placed_on"), On);
		return C;
	}

	TSharedRef<FJsonObject> InventoryJson(const FCubeInventory& Inventory)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		for (const auto& Pair : Inventory.Stacks) J->SetNumberField(CubeSpec::KindName(Pair.Key), Pair.Value);
		return J;
	}
}

// ── the door ─────────────────────────────────────────────────────────────────────────────────────

int32 ACubeWorldGameMode::WebPort() const
{
	int32 Port = 0;
	if (FParse::Value(FCommandLine::Get(), TEXT("-wsport="), Port) && Port > 0) return Port;
	const FString Given = FPlatformMisc::GetEnvironmentVariable(TEXT("CUBEWORLD_WS_PORT"));
	if (!Given.IsEmpty()) return FCString::Atoi(*Given);
	// On a pool machine the platform forwards one public TCP port to the port it allotted the process, so the door
	// listens on that port over TCP (Iris has it over UDP). Elsewhere ten above the game port: 7777 plays on 7787.
	const FString Allotted = FPlatformMisc::GetEnvironmentVariable(TEXT("PLAYSERV_ROOM_LISTEN_PORT"));
	if (!Allotted.IsEmpty() && FCString::Atoi(*Allotted) > 0) return FCString::Atoi(*Allotted);
	return GetWorld()->URL.Port + 10;
}

FString ACubeWorldGameMode::WebAddress() const
{
	FString Given;
	if (FParse::Value(FCommandLine::Get(), TEXT("-wsaddress="), Given) && !Given.IsEmpty()) return Given;
	Given = FPlatformMisc::GetEnvironmentVariable(TEXT("CUBEWORLD_WS_ADDRESS"));
	if (!Given.IsEmpty()) return Given;
	// On a pool machine browsers come in through the platform's TLS front on its public port (7777, as the C# rooms'),
	// which forwards to the door; the certificate is for the machine's name.
	const FString PublicHost = FPlatformMisc::GetEnvironmentVariable(TEXT("PLAYSERV_PUBLIC_HOST"));
	if (!PublicHost.IsEmpty())
	{
		const FString PublicPort = FPlatformMisc::GetEnvironmentVariable(TEXT("CUBEWORLD_WS_PUBLIC_PORT"));
		return FString::Printf(TEXT("wss://%s:%d"), *PublicHost, PublicPort.IsEmpty() ? 7777 : FCString::Atoi(*PublicPort));
	}
	return FString::Printf(TEXT("ws://%s:%d"), *ResolveHost(), Web.IsValid() ? Web->ListeningPort() : WebPort());
}

void ACubeWorldGameMode::OpenWebSocket()
{
	if (Web.IsValid()) return;
	TSharedPtr<FCubeWebSocketServer> Server = MakeShared<FCubeWebSocketServer>();
	if (!Server->Listen(WebPort())) return;
	Server->OnText.BindUObject(this, &ACubeWorldGameMode::OnWebText);
	Server->OnClosed.BindUObject(this, &ACubeWorldGameMode::OnWebClosed);
	Web = Server;
	ServerLog(FString::Printf(TEXT("browser clients may enter at %s"), *WebAddress()));
}

FCubeServerPlayer* ACubeWorldGameMode::PlayerOfWeb(int32 Client)
{
	if (Client == 0) return nullptr;
	for (auto& Pair : Players) if (Pair.Value->WebClient == Client) return Pair.Value.Get();
	return nullptr;
}

void ACubeWorldGameMode::WebSend(const FCubeServerPlayer& P, const TSharedRef<FJsonObject>& F)
{
	if (P.WebClient && Web.IsValid()) Web->Send(P.WebClient, ToText(F));
}

void ACubeWorldGameMode::WebBroadcast(const TSharedRef<FJsonObject>& F)
{
	if (!Web.IsValid()) return;
	FString Text;
	for (const auto& Pair : Players)
	{
		if (!Pair.Value->WebClient || !Pair.Value->bWelcomed) continue;
		if (Text.IsEmpty()) Text = ToText(F);
		Web->Send(Pair.Value->WebClient, Text);
	}
}

// The first frame is the handshake the C# server takes: who the player is and the ticket that admits them.
void ACubeWorldGameMode::OnWebText(int32 Client, const FString& Text)
{
	TSharedPtr<FJsonObject> Json;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) || !Json.IsValid()) return;
	FCubeServerPlayer* P = PlayerOfWeb(Client);
	if (!P)
	{
		if (!bServing || bClosing) { Web->Close(Client, 1008, TEXT("server_not_ready")); return; }
		FPlayServTicketVerdict Verdict;
		if (!bOffline)   // nobody hands out tickets offline
		{
			Verdict = PlayServ::Rooms::VerifyTicket(Str(Json, TEXT("reservationToken")));
			if (!Verdict.bAccepted)
			{
				ServerLog(FString::Printf(TEXT("a browser client's ticket was refused: %s"), *Verdict.Reason));
				Web->Close(Client, 1008, Verdict.Reason.IsEmpty() ? TEXT("reservation_invalid") : Verdict.Reason);
				return;
			}
		}
		FString Id = Verdict.PlayerId;
		if (Id.IsEmpty()) Id = Str(Json, TEXT("playerId"));
		if (Id.IsEmpty()) Id = FString::Printf(TEXT("local-%s-%d"), *ServerName, ++LocalIds);
		if (FCubeServerPlayer* Twice = PlayerById(Id)) RemovePlayer(Twice->Id);

		TSharedPtr<FCubeServerPlayer> Player = MakeShared<FCubeServerPlayer>();
		Player->WebClient = Client;
		Player->Id = Id;
		const FString Name = Str(Json, TEXT("displayName"));
		Player->Name = Name.IsEmpty() ? Id : Name.Left(32);
		// A browser client says "bombs" once it can show them; until then it is handed none.
		Player->bThrows = false;
		// A browser's hello names no position: a player walking in over a border stands where their server last saw them.
		Arrive(*Player, nullptr);
		Players.Add(Id, Player);
		// The other servers hear where they stand now, not on the next presence tick after the welcome.
		WritePresence(*Player);
		// The door is not an Unreal login, so the engine's PostLogin never tells the SDK: the player is admitted here,
		// and so is in the room's roster, the admin's player list and within an operator's reach.
		if (!bOffline) PlayServ::Rooms::AdmitVerified(Verdict);
		ServerLog(FString::Printf(TEXT("%s joined through the browser door"), *Player->Name));
		LoadInventoryAndWelcome(Id);
		return;
	}

	const FString Op = Str(Json, TEXT("op"));
	if (Op == TEXT("move"))
	{
		double Peak;
		const TOptional<double> SaidPeak = Json->TryGetNumberField(TEXT("peak"), Peak) ? TOptional<double>(Peak) : TOptional<double>();
		int32 Seq;
		const TOptional<int32> SaidSeq = Json->TryGetNumberField(TEXT("seq"), Seq) ? TOptional<int32>(Seq) : TOptional<int32>();
		OnMove(P, Num(Json, TEXT("x")), Num(Json, TEXT("y")), Num(Json, TEXT("z")), Num(Json, TEXT("yaw")), Num(Json, TEXT("pitch")), Flag(Json, TEXT("onGround")), Flag(Json, TEXT("sneaking")), Flag(Json, TEXT("sprinting")), SaidPeak, SaidSeq);
	}
	else if (Op == TEXT("dig")) OnDig(P, FMath::FloorToInt32(Num(Json, TEXT("x"))), FMath::FloorToInt32(Num(Json, TEXT("y"))), FMath::FloorToInt32(Num(Json, TEXT("z"))), Str(Json, TEXT("state")) == TEXT("start"));
	else if (Op == TEXT("place")) OnPlace(P, FMath::FloorToInt32(Num(Json, TEXT("x"))), FMath::FloorToInt32(Num(Json, TEXT("y"))), FMath::FloorToInt32(Num(Json, TEXT("z"))), (int32)Num(Json, TEXT("nx")), (int32)Num(Json, TEXT("ny")), (int32)Num(Json, TEXT("nz")), FName(*Str(Json, TEXT("kind"))));
	else if (Op == TEXT("attack")) OnAttack(P, Str(Json, TEXT("target")));
	else if (Op == TEXT("respawn")) OnRespawn(P);
	else if (Op == TEXT("throw")) OnThrow(P, Num(Json, TEXT("x")), Num(Json, TEXT("y")), Num(Json, TEXT("z")));
	else if (Op == TEXT("bombs")) P->bThrows = true;
}

void ACubeWorldGameMode::OnWebClosed(int32 Client)
{
	if (FCubeServerPlayer* P = PlayerOfWeb(Client)) RemovePlayer(P->Id);
}

/**
 * A player left, through either door: their dig stops, and their presence row goes once no other server has taken them
 * over (DeletePresence). Until the next server's first pose they stand where this server last saw them (KeepLastPose).
 */
void ACubeWorldGameMode::RemovePlayer(const FString& Id)
{
	const TSharedPtr<FCubeServerPlayer>* Found = Players.Find(Id);
	if (!Found) return;
	TSharedPtr<FCubeServerPlayer> Player = *Found;
	StopDig(*Player);
	ServerLog(FString::Printf(TEXT("%s left"), *Player->Name));
	// A browser's leave is the game's to report, as its join was (an Unreal client's goes through the engine's logout).
	if (Player->WebClient && !bOffline) PlayServ::Rooms::RemovePlayer(RoomName(), Id);
	UWorldPresence* Row = Player->PresenceRow.Get();
	TStrongObjectPtr<UWorldPresence> Keep(Row);
	KeepLastPose(*Player);
	Players.Remove(Id);
	if (Row) DeletePresence(Id, Row);
	PublishPlayers();
}

// ── what a browser client gets ───────────────────────────────────────────────────────────────────

void ACubeWorldGameMode::WebWelcome(FCubeServerPlayer& P)
{
	P.bWelcomed = true;
	P.bMoved = true;
	TSharedRef<FJsonObject> W = Frame(TEXT("welcome"));
	W->SetStringField(TEXT("server"), ServerName); W->SetStringField(TEXT("color"), Color()); W->SetNumberField(TEXT("region"), Region);
	TArray<TSharedPtr<FJsonValue>> RegionsJson;
	for (const FCubeRegionRep& R : Regions)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("region"), FString::FromInt(R.Region)); J->SetStringField(TEXT("server"), R.Server); J->SetStringField(TEXT("color"), R.Color); J->SetStringField(TEXT("room"), R.Room); J->SetStringField(TEXT("slug"), R.Slug);
		RegionsJson.Add(MakeShared<FJsonValueObject>(J));
	}
	W->SetArrayField(TEXT("regions"), RegionsJson);
	W->SetObjectField(TEXT("you"), PoseJson(PoseOf(P), Now()));
	W->SetNumberField(TEXT("width"), CubeSpec::Width_); W->SetNumberField(TEXT("depth"), CubeSpec::Depth); W->SetNumberField(TEXT("regionSize"), CubeSpec::RegionSize);
	W->SetNumberField(TEXT("minZ"), CubeSpec::MinZ); W->SetNumberField(TEXT("maxZ"), CubeSpec::MaxZ);
	TArray<TSharedPtr<FJsonValue>> LayersJson;
	for (const auto& Pair : CubeSpec::Layers()) { TSharedRef<FJsonObject> L = MakeShared<FJsonObject>(); L->SetNumberField(TEXT("z"), Pair.Key); L->SetStringField(TEXT("kind"), CubeSpec::KindName(Pair.Value)); LayersJson.Add(MakeShared<FJsonValueObject>(L)); }
	W->SetArrayField(TEXT("layers"), LayersJson);
	TArray<TSharedPtr<FJsonValue>> Trees;
	for (const FIntPoint& T : CubeTreeSpots()) { TSharedRef<FJsonObject> J = MakeShared<FJsonObject>(); J->SetNumberField(TEXT("x"), T.X); J->SetNumberField(TEXT("y"), T.Y); Trees.Add(MakeShared<FJsonValueObject>(J)); }
	W->SetArrayField(TEXT("trees"), Trees);
	TArray<TSharedPtr<FJsonValue>> Blocks;
	for (const FBlockDef& B : CubeSpec::Blocks())
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("kind"), B.Name); J->SetNumberField(TEXT("Hardness"), B.Hardness); J->SetBoolField(TEXT("NeedsTool"), B.bNeedsTool);
		J->SetBoolField(TEXT("Transparent"), B.bTransparent); J->SetBoolField(TEXT("Gravity"), B.bGravity);
		if (B.Drop != NAME_None) J->SetStringField(TEXT("Drop"), CubeSpec::KindName(B.Drop)); else J->SetField(TEXT("Drop"), MakeShared<FJsonValueNull>());
		J->SetNumberField(TEXT("breakTicks"), B.BreakTicks);
		Blocks.Add(MakeShared<FJsonValueObject>(J));
	}
	W->SetArrayField(TEXT("blocks"), Blocks);
	TArray<TSharedPtr<FJsonValue>> Hotbar;
	for (const FName& K : CubeSpec::Hotbar()) Hotbar.Add(MakeShared<FJsonValueString>(CubeSpec::KindName(K)));
	W->SetArrayField(TEXT("hotbar"), Hotbar);
	TArray<TSharedPtr<FJsonValue>> WorldJson;
	for (const auto& Pair : World.Overrides) WorldJson.Add(MakeShared<FJsonValueObject>(CubeJson(Pair.Key, Pair.Value.Kind, Pair.Value.By, Pair.Value.On)));
	W->SetArrayField(TEXT("world"), WorldJson);
	W->SetObjectField(TEXT("inventory"), InventoryJson(P.Inventory));
	W->SetNumberField(TEXT("tick"), (double)TickCount);
	TArray<TSharedPtr<FJsonValue>> BombsJson;
	for (const auto& Pair : Bombs)
	{
		if (Pair.Value.Record.State == TEXT("held") && Pair.Value.Record.Holder == P.Id) P.Bomb = Pair.Key;
		BombsJson.Add(MakeShared<FJsonValueObject>(BombJson(BombFrame(Pair.Value.Record, &Pair.Value))));
	}
	W->SetArrayField(TEXT("bombs"), BombsJson);
	WebSend(P, W);
	PublishPlayers();
}

void ACubeWorldGameMode::SendInventory(FCubeServerPlayer& P, bool bRefused)
{
	if (P.WebClient)
	{
		TSharedRef<FJsonObject> F = Frame(bRefused ? TEXT("refused") : TEXT("inventory"));
		if (bRefused) F->SetStringField(TEXT("op"), TEXT("place"));
		F->SetObjectField(TEXT("inventory"), InventoryJson(P.Inventory));
		WebSend(P, F);
		return;
	}
	if (ACubePlayerPawn* Pawn = P.Pawn.Get())
	{
		TArray<FCubeStackRep> Stacks;
		for (const auto& Pair : P.Inventory.Stacks) Stacks.Add({ CubeSpec::KindIndex(Pair.Key), Pair.Value });
		Pawn->ClientInventory(Stacks);
	}
}

void ACubeWorldGameMode::SendRespawn(FCubeServerPlayer& P)
{
	if (P.WebClient)
	{
		TSharedRef<FJsonObject> F = Frame(TEXT("respawn"));
		F->SetObjectField(TEXT("you"), PoseJson(PoseOf(P), Now()));
		WebSend(P, F);
		return;
	}
	if (ACubePlayerPawn* Pawn = P.Pawn.Get()) Pawn->ClientRespawn(P.X, P.Y, P.Z);
}

void ACubeWorldGameMode::Correct(FCubeServerPlayer& P, double X, double Y, double Z)
{
	const FCubeMoveCheck& Moves = P.Moves;
	if (P.WebClient)
	{
		TSharedRef<FJsonObject> F = Frame(TEXT("correct"));
		F->SetNumberField(TEXT("seq"), Moves.Seq);
		F->SetNumberField(TEXT("x"), Moves.X); F->SetNumberField(TEXT("y"), Moves.Y); F->SetNumberField(TEXT("z"), Moves.Z);
		WebSend(P, F);
	}
	else if (ACubePlayerPawn* Pawn = P.Pawn.Get()) Pawn->ClientCorrect(Moves.X, Moves.Y, Moves.Z, Moves.Seq);
	if (Now() - P.CorrectionLoggedAt < 5000) return;
	P.CorrectionLoggedAt = Now();
	const double Distance = FMath::Sqrt((X - Moves.X) * (X - Moves.X) + (Y - Moves.Y) * (Y - Moves.Y)) + FMath::Max(0.0, Z - Moves.Z);
	ServerLog(FString::Printf(TEXT("%s moved %.1f blocks too fast, put back (correction %d)"), *P.Name, Distance, Moves.Seq));
}

// ── what everyone gets ───────────────────────────────────────────────────────────────────────────

void ACubeWorldGameMode::BroadcastDig(const FString& PlayerId, int32 X, int32 Y, int32 Z, int32 Stage)
{
	if (State) State->MulticastDig(PlayerId, X, Y, Z, Stage);
	TSharedRef<FJsonObject> F = Frame(TEXT("dig"));
	F->SetStringField(TEXT("player"), PlayerId); F->SetNumberField(TEXT("x"), X); F->SetNumberField(TEXT("y"), Y); F->SetNumberField(TEXT("z"), Z); F->SetNumberField(TEXT("stage"), Stage);
	WebBroadcast(F);
}

void ACubeWorldGameMode::BroadcastHurt(const FString& PlayerId, double Health, double KX, double KY, double Strength, const FString& By)
{
	if (State) State->MulticastHurt(PlayerId, Health, KX, KY, Strength, By);
	TSharedRef<FJsonObject> F = Frame(TEXT("hurt"));
	F->SetStringField(TEXT("player"), PlayerId); F->SetNumberField(TEXT("health"), Health);
	if (By.IsEmpty()) F->SetField(TEXT("by"), MakeShared<FJsonValueNull>()); else F->SetStringField(TEXT("by"), By);
	F->SetNumberField(TEXT("kx"), KX); F->SetNumberField(TEXT("ky"), KY); F->SetNumberField(TEXT("strength"), Strength);
	WebBroadcast(F);
}

void ACubeWorldGameMode::BroadcastDeath(const FString& PlayerId, const FString& By)
{
	if (State) State->MulticastDeath(PlayerId, By);
	TSharedRef<FJsonObject> F = Frame(TEXT("death"));
	F->SetStringField(TEXT("player"), PlayerId);
	if (By.IsEmpty()) F->SetField(TEXT("by"), MakeShared<FJsonValueNull>()); else F->SetStringField(TEXT("by"), By);
	WebBroadcast(F);
}

void ACubeWorldGameMode::BroadcastBomb(const FCubeBombRep& B)
{
	if (State) State->MulticastBomb(B);
	if (!Web.IsValid()) return;
	// Only a browser client that said it shows bombs gets them, as the C# server keeps it.
	const FString Text = ToText(BombJson(B));
	for (const auto& Pair : Players)
		if (Pair.Value->WebClient && Pair.Value->bWelcomed && Pair.Value->bThrows) Web->Send(Pair.Value->WebClient, Text);
}

void ACubeWorldGameMode::WebBroadcastCubes(const TArray<FCubeChange>& Changes, const TArray<FCubeFall>& Falls, bool bRemote)
{
	if (!Web.IsValid()) return;
	TSharedRef<FJsonObject> F = Frame(TEXT("cubes"));
	F->SetBoolField(TEXT("remote"), bRemote);
	TArray<TSharedPtr<FJsonValue>> FallsJson;
	for (const FCubeFall& Fall : Falls)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("kind"), CubeSpec::KindName(Fall.Kind)); J->SetNumberField(TEXT("x"), Fall.X); J->SetNumberField(TEXT("y"), Fall.Y); J->SetNumberField(TEXT("fromZ"), Fall.FromZ); J->SetNumberField(TEXT("toZ"), Fall.ToZ);
		FallsJson.Add(MakeShared<FJsonValueObject>(J));
	}
	F->SetArrayField(TEXT("falls"), FallsJson);
	TArray<TSharedPtr<FJsonValue>> ChangesJson;
	for (const FCubeChange& C : Changes)
	{
		// A block back to the terrain (its row was deleted) is a delete, as the C# server passes one on.
		const bool bGenerated = C.Kind == NAME_None;
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("op"), bGenerated ? TEXT("delete") : TEXT("upsert"));
		J->SetObjectField(TEXT("cube"), CubeJson(C.At, bGenerated ? World.Voxels.Generated(C.At.X, C.At.Y, C.At.Z) : C.Kind, C.By, C.On));
		ChangesJson.Add(MakeShared<FJsonValueObject>(J));
	}
	F->SetArrayField(TEXT("changes"), ChangesJson);
	WebBroadcast(F);
}

void ACubeWorldGameMode::WebBroadcastPlayers()
{
	if (!Web.IsValid() || !State) return;
	TSharedRef<FJsonObject> F = Frame(TEXT("players"));
	TArray<TSharedPtr<FJsonValue>> List;
	const int64 T = Now();
	for (const FCubePresenceRep& P : State->Players) List.Add(MakeShared<FJsonValueObject>(PoseJson(P, T)));
	F->SetArrayField(TEXT("players"), List);
	WebBroadcast(F);
}

void ACubeWorldGameMode::WebBroadcastRegions()
{
	if (!Web.IsValid()) return;
	TSharedRef<FJsonObject> F = Frame(TEXT("regions"));
	TArray<TSharedPtr<FJsonValue>> List;
	for (const FCubeRegionRep& R : Regions)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("region"), FString::FromInt(R.Region)); J->SetStringField(TEXT("server"), R.Server); J->SetStringField(TEXT("color"), R.Color); J->SetStringField(TEXT("room"), R.Room); J->SetStringField(TEXT("slug"), R.Slug);
		List.Add(MakeShared<FJsonValueObject>(J));
	}
	F->SetArrayField(TEXT("regions"), List);
	WebBroadcast(F);
}
