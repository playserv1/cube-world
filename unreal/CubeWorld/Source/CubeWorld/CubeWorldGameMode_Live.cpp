// The server's live view of what the other servers and the functions write: the uplink's data subscriptions, one for
// each shared table and keyed as the C# servers key them (CubeWorldServer.Subscribe). The platform sends every upsert
// and every delete, whoever made it, the moment it lands, whatever its timestamp. What changed while a subscription
// was not in place (while the world loaded, or while the uplink reconnected) is not sent again, so a moment after
// every (re)subscription the table is read again. Which server holds which region is read in the heartbeat every 5 s,
// as the C# servers read it (LiveRegionsAsync).
//
// Until PSV-3009 presence and hits came only through windows on their timestamps (collection subscriptions on the
// realtime socket, with table polls behind them). A window keyed on the writer's clock missed rows stamped below it,
// stopped at 200 rows, saw no delete, and pushed its whole set again on every change.
#include "CubeWorldGameMode.h"
#include "CubeWorld.h"
#include "CubeEntities.h"
#include "PlayServ.h"
#include "Core/PlayServSubsystem.h"
#include "Rooms/PlayServRooms.h"

namespace
{
	constexpr TCHAR CubeEntity[] = TEXT("WorldCube");
	constexpr TCHAR InventoryEntity[] = TEXT("CubeInventory");
	constexpr TCHAR PresenceEntity[] = TEXT("WorldPresence");
	constexpr TCHAR HitEntity[] = TEXT("WorldHit");
	constexpr TCHAR BombEntity[] = TEXT("WorldBomb");
	/** How long after a subscription goes out its table is read again: the platform has taken it in by then. */
	constexpr float RereadDelaySeconds = 3.f;

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
}

// ── the rows ─────────────────────────────────────────────────────────────────────────────────────

FCubeBombRecord ACubeWorldGameMode::RecordOf(const TSharedPtr<FJsonObject>& Row)
{
	FCubeBombRecord R;
	R.Id = Str(Row, TEXT("bomb_id")); R.State = Str(Row, TEXT("state")); R.Holder = Str(Row, TEXT("holder"));
	R.X = Num(Row, TEXT("x")); R.Y = Num(Row, TEXT("y")); R.Z = Num(Row, TEXT("z"));
	R.VX = Num(Row, TEXT("vx")); R.VY = Num(Row, TEXT("vy")); R.VZ = Num(Row, TEXT("vz"));
	R.DroppedAt = (int64)Num(Row, TEXT("dropped_at")); R.At = (int64)Num(Row, TEXT("at"));
	return R;
}

FCubeElsewhere ACubeWorldGameMode::ElsewhereOf(const TSharedPtr<FJsonObject>& Row)
{
	FCubeElsewhere E;
	E.Pose.Id = Str(Row, TEXT("player_id")); E.Pose.Name = Str(Row, TEXT("name")); E.Pose.Server = Str(Row, TEXT("server")); E.Pose.Color = Str(Row, TEXT("color"));
	E.Pose.X = Num(Row, TEXT("x")); E.Pose.Y = Num(Row, TEXT("y")); E.Pose.Z = Num(Row, TEXT("z"));
	E.Pose.Yaw = Num(Row, TEXT("yaw")); E.Pose.Pitch = Num(Row, TEXT("pitch")); E.Pose.Health = Num(Row, TEXT("health"), 20);
	E.Pose.bSneaking = Num(Row, TEXT("sneaking")) == 1; E.Pose.bSprinting = Num(Row, TEXT("sprinting")) == 1;
	E.SeenAt = (int64)Num(Row, TEXT("seen_at"));
	return E;
}

FCubeElsewhere ACubeWorldGameMode::ElsewhereOf(const UWorldPresence* Row)
{
	FCubeElsewhere E;
	E.Pose.Id = Row->player_id; E.Pose.Name = Row->name; E.Pose.Server = Row->server; E.Pose.Color = Row->color;
	E.Pose.X = Row->x; E.Pose.Y = Row->y; E.Pose.Z = Row->z; E.Pose.Yaw = Row->yaw; E.Pose.Pitch = Row->pitch; E.Pose.Health = Row->health;
	E.Pose.bSneaking = Row->sneaking == 1; E.Pose.bSprinting = Row->sprinting == 1;
	E.SeenAt = Row->seen_at;
	return E;
}

FCubeHitRecord ACubeWorldGameMode::HitOf(const TSharedPtr<FJsonObject>& Row)
{
	FCubeHitRecord H;
	H.Id = Str(Row, TEXT("hit_id")); H.Victim = Str(Row, TEXT("victim")); H.Attacker = Str(Row, TEXT("attacker"));
	H.Damage = Num(Row, TEXT("damage")); H.KX = Num(Row, TEXT("kx")); H.KY = Num(Row, TEXT("ky")); H.Strength = Num(Row, TEXT("strength"));
	H.At = (int64)Num(Row, TEXT("at"));
	return H;
}

FCubeHitRecord ACubeWorldGameMode::HitOf(const UWorldHit* Row)
{
	FCubeHitRecord H;
	H.Id = Row->hit_id; H.Victim = Row->victim; H.Attacker = Row->attacker;
	H.Damage = Row->damage; H.KX = Row->kx; H.KY = Row->ky; H.Strength = Row->strength;
	H.At = Row->at;
	return H;
}

// ── the uplink's data subscriptions ──────────────────────────────────────────────────────────────

void ACubeWorldGameMode::SubscribeUplink()
{
	UPlayServRooms* Rooms = UPlayServSubsystem::Get() ? UPlayServSubsystem::Get()->GetRooms() : nullptr;
	if (!Rooms) return;
	Rooms->OnDataUpdate.RemoveAll(this);
	Rooms->OnDataSubscribed.RemoveAll(this);
	Rooms->OnDataUpdate.AddUObject(this, &ACubeWorldGameMode::HandleDataUpdate);
	Rooms->OnDataSubscribed.AddUObject(this, &ACubeWorldGameMode::HandleDataSubscribed);
	Rooms->SubscribeData(CubeEntity, TEXT("field:key"));
	// The old server's last write after a crossing and the refill function's top-ups reach a player who is here.
	Rooms->SubscribeData(InventoryEntity, TEXT("field:player_id"));
	Rooms->SubscribeData(PresenceEntity, TEXT("field:player_id"));
	Rooms->SubscribeData(HitEntity, TEXT("field:hit_id"));
	// Every row any server or the drop function writes, whatever its `at` (PSV-2977): a window on `at` missed a fizzle
	// written with an old time, and every bomb it missed ending stayed in play here as a ghost.
	Rooms->SubscribeData(BombEntity, TEXT("field:bomb_id"));
}

/**
 * The subscription went out, first once the room was open, then on every new uplink socket: what changed before it
 * was in place was not heard, so the table is read again. Not at once: the SDK reports the subscription as it sends
 * it, the platform takes it in a moment later and acknowledges nothing, and a row written between a read made at once
 * and that moment was neither read nor sent. On dev a new server kept a fizzled bomb that way, free and falling for
 * as long as it ran: the drop function fizzled it in the same tenth of a second as the server subscribed. A row read
 * and also heard changes nothing.
 */
void ACubeWorldGameMode::HandleDataSubscribed(const FString& Entity)
{
	if (bClosing) return;
	if (Entity == CubeEntity && CubesSubscribedAt == 0) CubesSubscribedAt = Now();
	const FString Table = Entity;
	GetWorldTimerManager().SetTimer(RereadTimers.FindOrAdd(Entity), FTimerDelegate::CreateWeakLambda(this, [this, Table]() { RereadTable(Table); }), RereadDelaySeconds, false);
}

void ACubeWorldGameMode::RereadTable(const FString& Entity)
{
	if (bClosing) return;
	if (Entity == BombEntity) ReloadBombs();
	else if (Entity == PresenceEntity) ReloadPresence();
	else if (Entity == HitEntity) ReloadHits();
	else if (Entity == CubeEntity) ReconcileCubes();
}

void ACubeWorldGameMode::HandleDataUpdate(const FPlayServDataUpdate& Update)
{
	if (Update.Entity == PresenceEntity)
	{
		const FString Id = Str(Update.Data, TEXT("player_id"));
		// They left, or a server deleted the row as they crossed: gone until their next pose (HearPresence on the C# side).
		if (Update.IsDelete()) { Elsewhere.Remove(Id.IsEmpty() ? Update.Id : Id); return; }
		if (!Update.Data.IsValid()) return;
		FCubeElsewhere Pose = ElsewhereOf(Update.Data);
		if (Pose.Pose.Id.IsEmpty()) Pose.Pose.Id = Update.Id;
		HearPose(Pose);
		return;
	}
	if (Update.Entity == HitEntity)
	{
		// A delete is a hit applied, here or on its victim's server.
		if (Update.IsDelete() || !Update.Data.IsValid()) return;
		FCubeHitRecord Hit = HitOf(Update.Data);
		if (Hit.Id.IsEmpty()) Hit.Id = Update.Id;
		HearHit(Hit, nullptr);
		return;
	}
	if (Update.Entity == BombEntity)
	{
		// The sweep's deletes take nothing out of play: a bomb is deleted only two minutes after it went off.
		if (Update.IsDelete() || !Update.Data.IsValid()) return;
		const FCubeBombRecord R = RecordOf(Update.Data);
		if (!R.Id.IsEmpty()) OnBomb(R, false);
		return;
	}
	if (Update.Entity == InventoryEntity)
	{
		// Only a whole row is a row to merge: one without its stacks would read as the starting stacks.
		if (!bServing || Update.IsDelete() || !Update.Data.IsValid() || Str(Update.Data, TEXT("stacks")).IsEmpty()) return;
		const FString Id = Str(Update.Data, TEXT("player_id"));
		HearInventory(Id.IsEmpty() ? Update.Id : Id, FCubeInventory::Parse(Str(Update.Data, TEXT("stacks"))));
		return;
	}
	if (Update.Entity != CubeEntity || !bServing) return;
	bCubeUpdatesHeard = true;
	const TSharedPtr<FJsonObject>& Row = Update.Data;
	// The block is the row's x, y and z; one that came without them is found by its key, or by the record's id.
	FIntVector Where;
	double X = 0, Y = 0, Z = 0;
	if (Row.IsValid() && Row->TryGetNumberField(TEXT("x"), X) && Row->TryGetNumberField(TEXT("y"), Y) && Row->TryGetNumberField(TEXT("z"), Z)) Where = FIntVector((int32)X, (int32)Y, (int32)Z);
	else if (!FCubeServerWorld::ParseKey(Str(Row, TEXT("key")), Where) && !FCubeServerWorld::ParseKey(Update.Id, Where)) return;
	const int64 At = (int64)Num(Row, TEXT("at"));
	const FString By = Str(Row, TEXT("placed_by")), On = Str(Row, TEXT("placed_on"));
	if (Update.IsDelete()) { Bury(Where, At, By, On); return; }
	const FName Kind(*Str(Row, TEXT("kind")));
	if (Kind == NAME_None) return;
	if (World.Apply(Where, Kind, By, On, At, nullptr) && On != ServerName) Heard.Add({ Where, Kind, By, On });
}

// ── the players and hits of the other servers ────────────────────────────────────────────────────

ECubePoseHeard ACubeWorldGameMode::MergePose(TMap<FString, FCubeElsewhere>& Poses, const FCubeElsewhere& Pose)
{
	const FCubeElsewhere* Known = Poses.Find(Pose.Pose.Id);
	if (Known && Known->SeenAt > Pose.SeenAt) return ECubePoseHeard::Older;
	const bool bHurt = Known && CubeWasHurt(Known->Pose.Health, Known->SeenAt, Pose.Pose.Health, Pose.SeenAt);
	Poses.Add(Pose.Pose.Id, Pose);
	return bHurt ? ECubePoseHeard::Hurt : ECubePoseHeard::Taken;
}

// A player hurt elsewhere (a hit from here went over as a WorldHit, a fall or a blast happened there) is flashed for
// this server's players too: their own server tells only its players. The health shown stays the victim's server's.
void ACubeWorldGameMode::HearPose(const FCubeElsewhere& Pose)
{
	// This server's own writes come back over the uplink like anyone's, and a player here is this server's to say.
	if (Pose.Pose.Id.IsEmpty() || Pose.Pose.Server == ServerName || Players.Contains(Pose.Pose.Id)) return;
	if (MergePose(Elsewhere, Pose) == ECubePoseHeard::Hurt) BroadcastHurt(Pose.Pose.Id, Pose.Pose.Health, 0, 0, 0, FString());
}

void ACubeWorldGameMode::ReloadPresence()
{
	if (bPresenceReloading || bOffline) return;
	bPresenceReloading = true;
	TWeakObjectPtr<ACubeWorldGameMode> Weak(this);
	// Only the poses that still count: the table keeps a row for every player who ever left without its delete.
	PlayServ::Data::LoadAll<UWorldPresence>(FPlayServFilter::Where(TEXT("seen_at")).GreaterThan((double)(Now() - CubePresenceTtlMs)), [Weak](bool bOk, TArray<UWorldPresence*> Rows, const FPlayServError& Error)
	{
		if (!Weak.IsValid()) return;
		Weak->bPresenceReloading = false;
		// Every pose is written again within 2 s, so one read missed is made up for soon.
		if (!bOk) { Weak->ServerLog(FString::Printf(TEXT("players elsewhere not read again: %s"), *Error.Message)); return; }
		for (const UWorldPresence* Row : Rows) Weak->HearPose(ElsewhereOf(Row));
	});
}

/**
 * A hit lands once, on a player here, within 5 s of being written, as on the C# servers (HearHit): the table keeps the
 * hits whose victim had moved on, and one heard later, when the victim is back, or read again on a new subscription,
 * does not land then. The uplink brings each hit as it is written, so two hits 100 ms apart land in their order
 * (PSV-2978); a read of the table is put in that order first.
 */
void ACubeWorldGameMode::HearHit(const FCubeHitRecord& Hit, UWorldHit* Row)
{
	FCubeServerPlayer* Victim = PlayerById(Hit.Victim);
	if (!Victim || Hit.Id.IsEmpty() || HitsApplied.Contains(Hit.Id) || !CubeHitIsFresh(Hit.At, Now())) return;
	if (HitsApplied.Num() >= 1000) HitsApplied.Empty();
	HitsApplied.Add(Hit.Id);
	Hurt(*Victim, Hit.Damage, true, Hit.KX, Hit.KY, Hit.Strength, Hit.Attacker);
	if (Row)
	{
		TStrongObjectPtr<UWorldHit> Keep(Row);
		PlayServ::Data::Delete(Row, FPlayServSimpleCallback::CreateLambda([Keep](bool, const FPlayServError&) {}));
	}
	else DeleteHit(Hit.Id);
}

// A hit heard over the uplink carries its fields, not its record: it is deleted by its hit id. A C# server's hit is
// announced to the other servers before it is stored, so a delete that finds nothing tries once more, 2 s later.
void ACubeWorldGameMode::DeleteHit(const FString& HitId, int32 Attempt)
{
	if (bOffline) return;
	TWeakObjectPtr<ACubeWorldGameMode> Weak(this);
	PlayServ::Data::DeleteAll<UWorldHit>(FPlayServFilter::Where(TEXT("hit_id")).EqualTo(HitId), FPlayServDeleteAllCallback::CreateLambda([Weak, HitId, Attempt](bool bOk, const FPlayServDeleteAllResult& Result, const FPlayServError&)
	{
		if (!Weak.IsValid() || Attempt > 0 || (bOk && Result.DeletedCount > 0)) return;
		FTimerHandle Later;
		Weak->GetWorldTimerManager().SetTimer(Later, FTimerDelegate::CreateWeakLambda(Weak.Get(), [Weak, HitId]() { if (Weak.IsValid()) Weak->DeleteHit(HitId, 1); }), 2.f, false);
	}));
}

void ACubeWorldGameMode::ReloadHits()
{
	if (bHitsReloading || bOffline || Players.Num() == 0) return;
	bHitsReloading = true;
	TWeakObjectPtr<ACubeWorldGameMode> Weak(this);
	PlayServ::Data::LoadAll<UWorldHit>(FPlayServFilter::Where(TEXT("at")).GreaterThan((double)(Now() - CubeHitTtlMs)), [Weak](bool bOk, TArray<UWorldHit*> Rows, const FPlayServError& Error)
	{
		if (!Weak.IsValid()) return;
		Weak->bHitsReloading = false;
		if (!bOk) { Weak->ServerLog(FString::Printf(TEXT("hits not read again: %s"), *Error.Message)); return; }
		Rows.Sort([](const UWorldHit& A, const UWorldHit& B) { return A.at < B.at; });
		for (UWorldHit* Row : Rows) Weak->HearHit(HitOf(Row), Row);
	});
}

// ── the blocks ───────────────────────────────────────────────────────────────────────────────────

/**
 * The table as it is now, against the blocks this server holds: a block whose row is gone goes back to the terrain,
 * a row this server did not know of, or knew otherwise, is taken. A block that changed here after the read began is
 * newer than what the read found, and a write of this server's own still on its way is not in the table yet: both
 * stay as they are.
 */
void ACubeWorldGameMode::ReconcileCubes()
{
	if (bReconciling) { bReconcileAgain = true; return; }
	bReconciling = true;
	const uint64 AsOf = World.Version;
	TWeakObjectPtr<ACubeWorldGameMode> Weak(this);
	PlayServ::Data::LoadAll<UWorldCube>(FPlayServFilter::None(), [Weak, AsOf](bool bOk, TArray<UWorldCube*> Rows, const FPlayServError& Error)
	{
		if (!Weak.IsValid()) return;
		ACubeWorldGameMode* Self = Weak.Get();
		Self->bReconciling = false;
		if (!bOk)
		{
			Self->ServerLog(FString::Printf(TEXT("blocks not read again, trying in 10 s: %s"), *Error.Message));
			FTimerHandle H;
			Self->GetWorldTimerManager().SetTimer(H, Self, &ACubeWorldGameMode::ReconcileCubes, 10.f, false);
		}
		else if (!Self->bClosing)
		{
			TSet<FIntVector> Found;
			int32 Taken = 0, Gone = 0;
			for (UWorldCube* Row : Rows)
			{
				const FIntVector At(Row->x, Row->y, Row->z);
				Found.Add(At);
				const FCubeOverride* Known = Self->World.Overrides.Find(At);
				if ((Known && Known->Version > AsOf) || Self->IsBuried(At, Row->at)) continue;
				const FName Kind(*Row->kind);
				if (Kind == NAME_None || !Self->World.Apply(At, Kind, Row->placed_by, Row->placed_on, Row->at, Row)) continue;
				Taken++;
				if (Row->placed_on != Self->ServerName) Self->Heard.Add({ At, Kind, Row->placed_by, Row->placed_on });
			}
			for (const FIntVector& At : Self->World.Missing(Found, AsOf))
			{
				if (Self->WritesInFlight.Contains(At)) continue;
				const FCubeOverride Known = Self->World.Overrides.FindChecked(At);
				Self->Bury(At, Known.At, Known.By, Known.On);
				Gone++;
			}
			Self->ServerLog(FString::Printf(TEXT("blocks read again: %d rows, %d blocks back to the terrain, %d taken from the table"), Rows.Num(), Gone, Taken));
		}
		if (Self->bReconcileAgain) { Self->bReconcileAgain = false; Self->ReconcileCubes(); }
	});
}

void ACubeWorldGameMode::Bury(const FIntVector& At, int64 RowAt, const FString& By, const FString& On)
{
	const FCubeOverride* Known = World.Overrides.Find(At);
	FCubeTombstone& Tomb = Tombstones.FindOrAdd(At);
	Tomb.At = FMath::Max3(Tomb.At, RowAt, Known ? Known->At : (int64)0);
	Tomb.HeardAt = Now();
	if (World.Forget(At)) Heard.Add({ At, NAME_None, By, On });
}

bool ACubeWorldGameMode::IsBuried(const FIntVector& At, int64 RowAt) const
{
	const FCubeTombstone* Tomb = Tombstones.Find(At);
	return Tomb && RowAt <= Tomb->At;
}
