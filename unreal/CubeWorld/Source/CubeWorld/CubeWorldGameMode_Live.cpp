// The server's live view of the other servers' writes: collection subscriptions on the platform's realtime socket
// (CubeLiveTables). Each table is watched through a window on its timestamp, so the set the platform pushes stays
// small: the blocks changed since the window opened, the players seen in the last seconds, the hits and the bomb
// moves since the server started. A window that fills up is moved forward. The polls in CubeWorldGameMode.cpp stay
// as the fallback while the socket is down.
#include "CubeWorldGameMode.h"
#include "CubeWorld.h"
#include "CubeLiveTables.h"
#include "CubeEntities.h"
#include "PlayServ.h"

namespace
{
	constexpr int32 WindowFull = 150;
	constexpr int64 PresenceWindowMs = 5000, PresenceWindowRefreshMs = 30000;

	double Num(const TSharedPtr<FJsonObject>& Row, const TCHAR* Field, double Default = 0)
	{
		double V = Default;
		if (Row.IsValid()) Row->TryGetNumberField(FStringView(Field), V);
		return V;
	}

	FString Str(const TSharedPtr<FJsonObject>& Row, const TCHAR* Field)
	{
		FString V;
		if (Row.IsValid()) Row->TryGetStringField(FStringView(Field), V);
		return V;
	}

	FString Since(const TCHAR* Entity, const TCHAR* Field, int64 After)
	{
		return FString::Printf(TEXT("%s(where: { %s: { gt: %lld } }, limit: 200) { * }"), Entity, Field, After);
	}
}

void ACubeWorldGameMode::OpenLiveTables()
{
	LiveTables = MakeShared<FCubeLiveTables>();
	SubscribeLiveCubes();
	SubscribeLivePresence();
	SubscribeLiveHits();
	SubscribeLiveBombs();
	SubscribeLiveRegions();
}

void ACubeWorldGameMode::SubscribeLiveCubes()
{
	if (LiveCubes) LiveTables->Unsubscribe(LiveCubes);
	LiveCubes = LiveTables->Subscribe(TEXT("WorldCube"), Since(TEXT("WorldCube"), TEXT("at"), LastCubeAt - 1), FCubeLiveTables::FOnLiveRows::CreateUObject(this, &ACubeWorldGameMode::OnLiveCubes));
}

void ACubeWorldGameMode::SubscribeLivePresence()
{
	if (LivePresence) LiveTables->Unsubscribe(LivePresence);
	LivePresenceSince = Now() - PresenceWindowMs;
	LivePresence = LiveTables->Subscribe(TEXT("WorldPresence"), Since(TEXT("WorldPresence"), TEXT("seen_at"), LivePresenceSince), FCubeLiveTables::FOnLiveRows::CreateUObject(this, &ACubeWorldGameMode::OnLivePresence));
}

void ACubeWorldGameMode::SubscribeLiveHits()
{
	if (LiveHits) LiveTables->Unsubscribe(LiveHits);
	LiveHitsSince = Now() - 5000;
	LiveHits = LiveTables->Subscribe(TEXT("WorldHit"), Since(TEXT("WorldHit"), TEXT("at"), LiveHitsSince), FCubeLiveTables::FOnLiveRows::CreateUObject(this, &ACubeWorldGameMode::OnLiveHits));
}

void ACubeWorldGameMode::SubscribeLiveBombs()
{
	if (LiveBombs) LiveTables->Unsubscribe(LiveBombs);
	LiveBombs = LiveTables->Subscribe(TEXT("WorldBomb"), Since(TEXT("WorldBomb"), TEXT("at"), LastBombAt - 1), FCubeLiveTables::FOnLiveRows::CreateUObject(this, &ACubeWorldGameMode::OnLiveBombs));
}

void ACubeWorldGameMode::SubscribeLiveRegions()
{
	if (LiveRegions) LiveTables->Unsubscribe(LiveRegions);
	LiveRegions = LiveTables->Subscribe(TEXT("WorldRegion"), TEXT("WorldRegion(limit: 200) { * }"), FCubeLiveTables::FOnLiveRows::CreateUObject(this, &ACubeWorldGameMode::OnLiveRegions));
}

void ACubeWorldGameMode::OnLiveCubes(const TArray<TSharedPtr<FJsonObject>>& Rows)
{
	for (const TSharedPtr<FJsonObject>& Row : Rows)
	{
		const int64 At = (int64)Num(Row, TEXT("at"));
		LastCubeAt = FMath::Max(LastCubeAt, At);
		const FIntVector Where((int32)Num(Row, TEXT("x")), (int32)Num(Row, TEXT("y")), (int32)Num(Row, TEXT("z")));
		const FName Kind(*Str(Row, TEXT("kind")));
		const FString On = Str(Row, TEXT("placed_on"));
		if (World.Apply(Where, Kind, Str(Row, TEXT("placed_by")), On, At, nullptr) && On != ServerName) Heard.Add({ Where, Kind, Str(Row, TEXT("placed_by")), On });
	}
	// A full window would hide the next change: move it up to the newest change heard.
	if (Rows.Num() >= WindowFull) SubscribeLiveCubes();
}

void ACubeWorldGameMode::OnLivePresence(const TArray<TSharedPtr<FJsonObject>>& Rows)
{
	TMap<FString, FCubeElsewhere> Fresh;
	for (const TSharedPtr<FJsonObject>& Row : Rows)
	{
		const FString Id = Str(Row, TEXT("player_id"));
		if (Str(Row, TEXT("server")) == ServerName || Players.Contains(Id)) continue;
		FCubeElsewhere E;
		E.Pose.Id = Id; E.Pose.Name = Str(Row, TEXT("name")); E.Pose.Server = Str(Row, TEXT("server")); E.Pose.Color = Str(Row, TEXT("color"));
		E.Pose.X = Num(Row, TEXT("x")); E.Pose.Y = Num(Row, TEXT("y")); E.Pose.Z = Num(Row, TEXT("z"));
		E.Pose.Yaw = Num(Row, TEXT("yaw")); E.Pose.Pitch = Num(Row, TEXT("pitch")); E.Pose.Health = Num(Row, TEXT("health"), 20);
		E.Pose.bSneaking = Num(Row, TEXT("sneaking")) == 1; E.Pose.bSprinting = Num(Row, TEXT("sprinting")) == 1;
		E.SeenAt = (int64)Num(Row, TEXT("seen_at"));
		const FCubeElsewhere* Known = Fresh.Find(Id);
		if (!Known || E.SeenAt > Known->SeenAt) Fresh.Add(Id, E);
	}
	Elsewhere = Fresh;
	PublishPlayers();
	// The window holds everyone seen since it opened; every half minute, or when it fills, it opens again at now.
	if (Rows.Num() >= WindowFull || Now() - LivePresenceSince > PresenceWindowRefreshMs) SubscribeLivePresence();
}

void ACubeWorldGameMode::OnLiveHits(const TArray<TSharedPtr<FJsonObject>>& Rows)
{
	for (const TSharedPtr<FJsonObject>& Row : Rows)
	{
		const FString HitId = Str(Row, TEXT("hit_id"));
		FCubeServerPlayer* Victim = PlayerById(Str(Row, TEXT("victim")));
		if (!Victim || HitsApplied.Contains(HitId)) continue;
		HitsApplied.Add(HitId);
		Hurt(*Victim, Num(Row, TEXT("damage")), true, Num(Row, TEXT("kx")), Num(Row, TEXT("ky")), Num(Row, TEXT("strength")), Str(Row, TEXT("attacker")));
		const FString RecordId = Str(Row, TEXT("id"));
		if (!RecordId.IsEmpty()) PlayServ::Data::DeleteById<UWorldHit>(RecordId, FPlayServSimpleCallback::CreateLambda([](bool, const FPlayServError&) {}));
	}
	if (HitsApplied.Num() > 1000) HitsApplied.Empty();
	// Applied hits are deleted, so the set stays small; a window that still fills up (other servers' hits) moves on.
	if (Rows.Num() >= WindowFull) SubscribeLiveHits();
}

void ACubeWorldGameMode::OnLiveBombs(const TArray<TSharedPtr<FJsonObject>>& Rows)
{
	TArray<TSharedPtr<FJsonObject>> Sorted = Rows;
	Sorted.Sort([](const TSharedPtr<FJsonObject>& A, const TSharedPtr<FJsonObject>& B) { return Num(A, TEXT("at")) < Num(B, TEXT("at")); });
	for (const TSharedPtr<FJsonObject>& Row : Sorted)
	{
		FCubeBombRecord R;
		R.Id = Str(Row, TEXT("bomb_id")); R.State = Str(Row, TEXT("state")); R.Holder = Str(Row, TEXT("holder"));
		R.X = Num(Row, TEXT("x")); R.Y = Num(Row, TEXT("y")); R.Z = Num(Row, TEXT("z"));
		R.VX = Num(Row, TEXT("vx")); R.VY = Num(Row, TEXT("vy")); R.VZ = Num(Row, TEXT("vz"));
		R.DroppedAt = (int64)Num(Row, TEXT("dropped_at")); R.At = (int64)Num(Row, TEXT("at"));
		LastBombAt = FMath::Max(LastBombAt, R.At);
		if (!R.Id.IsEmpty()) OnBomb(R, false);
	}
	if (Rows.Num() >= WindowFull) SubscribeLiveBombs();
}

void ACubeWorldGameMode::OnLiveRegions(const TArray<TSharedPtr<FJsonObject>>& Rows)
{
	TArray<FCubeRegionRep> LiveRows;
	const int64 T = Now();
	for (const TSharedPtr<FJsonObject>& Row : Rows)
		if (T - (int64)Num(Row, TEXT("seen_at")) < 30000)
			LiveRows.Add({ FCString::Atoi(*Str(Row, TEXT("region"))), Str(Row, TEXT("room")), Str(Row, TEXT("color")), Str(Row, TEXT("server")) });
	LiveRows.Sort([](const FCubeRegionRep& A, const FCubeRegionRep& B) { return A.Region < B.Region; });
	Regions = LiveRows;
	if (State) State->Regions = LiveRows;
}
