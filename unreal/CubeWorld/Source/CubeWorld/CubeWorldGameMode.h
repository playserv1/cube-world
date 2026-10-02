// The dedicated server: the authority Minecraft's server is. It signs in to the platform, loads the world, claims a
// free region, opens its room through the PlayServ SDK and admits players by their tickets. It checks reach and the
// face a block is placed against, times every dig by the block's hardness, deals damage and knockback, flies bombs,
// and ticks 20 times a second. Movement is simulated by the client and reported back, as Minecraft clients do.
// Every change goes through platform data, and the other servers' changes come back the same way, so three servers
// share one world. A port of CubeWorld.Server/CubeWorldServer.cs.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "CubeServerWorld.h"
#include "CubeWorldState.h"
#include "CubeEntities.h"
#include "Rooms/PlayServRoomsTypes.h"
#include "UObject/StrongObjectPtr.h"
#include "Dom/JsonObject.h"
#include "CubeWorldGameMode.generated.h"

class ACubePlayerPawn;
struct FPlayServError;
struct FPlayServDataUpdate;

/** A WorldBomb row as the server follows it. */
struct FCubeBombRecord
{
	FString Id, State, Holder;
	double X = 0, Y = 0, Z = 0, VX = 0, VY = 0, VZ = 0;
	int64 DroppedAt = 0, At = 0;

	static int32 Rank(const FString& State) { return State == TEXT("free") ? 0 : State == TEXT("held") ? 1 : State == TEXT("flying") ? 2 : 3; }
	bool IsOver() const { return Rank(State) == 3; }
};

/** A WorldHit row: a hit on a player this server hosts, from another server. */
struct FCubeHitRecord
{
	FString Id, Victim, Attacker;
	double Damage = 0, KX = 0, KY = 0, Strength = 0;
	int64 At = 0;
};

struct FCubeLiveBomb
{
	FCubeBombRecord Record;
	/** The height of a free one now. */
	double Z = 0;
	/** The path of one this server threw. */
	double P[3] = { 0, 0, 0 }, V[3] = { 0, 0, 0 };
	bool bOwned = false;
	int32 Age = 0;
};

struct FCubeDig
{
	int32 X = 0, Y = 0, Z = 0;
	int64 StartTick = 0;
	int32 Ticks = 0;
	int32 Stage = 0;
};

/** A player on this server. */
struct FCubeServerPlayer
{
	TWeakObjectPtr<ACubePlayerPawn> Pawn;
	TWeakObjectPtr<AController> Controller;
	/** The WebSocket client this player is, when they came in through that door; 0 for an Unreal client. */
	int32 WebClient = 0;
	/** The client shows bombs and reads batched cube frames (every Unreal client; a browser once it says so). */
	bool bThrows = true;
	FString Id, Name;
	double X = 0, Y = 0, Z = 0, Yaw = 0, Pitch = 0, Health = 20;
	bool bSneaking = false, bSprinting = false;
	bool bMoved = false, bDead = false, bWelcomed = false;
	FCubePlayerFall Fall;
	FCubeMoveCheck Moves;
	int64 CorrectionLoggedAt = 0;
	/** Where Arrive guessed the player stands, from what (their hello, another server's presence, or the spawn) and when:
	 *  the first move is logged against it, the moment a crossing is judged. */
	FVector Guess = FVector::ZeroVector;
	const TCHAR* GuessFrom = TEXT("the spawn");
	int64 ArrivedAt = 0;
	bool bFirstMoveLogged = false;
	int64 LastAttackTick = -1000000, LastHurtTick = -1000000;
	TOptional<FCubeDig> Dig;
	/** When the player stepped out of this server's region (ms); 0 while they stand in it. */
	int64 OutsideSince = 0;
	/** The bomb in the player's hand. A player holds one at a time and can only throw it. */
	FString Bomb;
	FCubeInventory Inventory;
	TStrongObjectPtr<UCubeInventory> InventoryRow;
	FCubeInventorySync InventorySync;
	/** The row has been read (or found missing) and the inventory is the player's own; until then a row heard waits. */
	bool bInventoryRead = false;
	TOptional<FCubeInventory> InventoryHeardWhileReading;
	/** Saves refused in a row because the row changed meanwhile; each one reads the row again and merges. */
	int32 InventoryConflicts = 0;
	TStrongObjectPtr<UWorldPresence> PresenceRow;
	int64 PresenceWrittenAt = 0;
	bool bPresenceBusy = false;
};

/** A player another server hosts, as last heard. */
struct FCubeElsewhere
{
	FCubePresenceRep Pose;
	int64 SeenAt = 0;
};

/** What became of a pose heard of a player elsewhere (ACubeWorldGameMode::MergePose). */
enum class ECubePoseHeard : uint8
{
	/** Older than the pose already known: a push the platform sent late. */
	Older,
	Taken,
	/** Taken, and it shows the player hurt since the pose before (CubeWasHurt). */
	Hurt,
};

/** A block whose row was deleted: the `at` of the row that went, and when the delete was heard. */
struct FCubeTombstone
{
	int64 At = 0;
	int64 HeardAt = 0;
};

UCLASS()
class CUBEWORLD_API ACubeWorldGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ACubeWorldGameMode();
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	virtual void PreLogin(const FString& Options, const FString& Address, const FUniqueNetIdRepl& UniqueId, FString& ErrorMessage) override;
	virtual void PostLogin(APlayerController* NewPlayer) override;
	/** Offline, the id the client names itself with (?cubeplayer=) is read from its login options here. */
	virtual FString InitNewPlayer(APlayerController* NewPlayerController, const FUniqueNetIdRepl& UniqueId, const FString& Options, const FString& Portal = TEXT("")) override;
	virtual void Logout(AController* Exiting) override;

	/** True in a dedicated server that has claimed its region and opened its room. */
	bool IsServing() const { return bServing; }

	// ---- what the players ask (from the pawn's server RPCs) ---------------------------------
	void OnHello(ACubePlayerPawn* Pawn, const FString& Name, bool bCross, double X, double Y, double Z);
	/** SaidPeak: the client's highest point since it last stood on the ground (FCubePlayerFall::Step); unset when it said none.
	 *  SaidSeq: the last correction the client took (FCubeMoveCheck), from a client that numbers its moves. */
	void OnMove(FCubeServerPlayer* P, double X, double Y, double Z, double Yaw, double Pitch, bool bOnGround, bool bSneaking, bool bSprinting, TOptional<double> SaidPeak = TOptional<double>(), TOptional<int32> SaidSeq = TOptional<int32>());
	void OnDig(FCubeServerPlayer* P, int32 X, int32 Y, int32 Z, bool bStart);
	void OnPlace(FCubeServerPlayer* P, int32 X, int32 Y, int32 Z, int32 NX, int32 NY, int32 NZ, FName Kind);
	void OnAttack(FCubeServerPlayer* P, const FString& Target);
	void OnRespawn(FCubeServerPlayer* P);
	void OnThrow(FCubeServerPlayer* P, double DX, double DY, double DZ);
	FCubeServerPlayer* PlayerOf(ACubePlayerPawn* Pawn);

	// ---- the WebSocket door: browser clients, in the C# server's JSON frames ---------------------------
	void OpenWebSocket();
	void OnWebText(int32 Client, const FString& Text);
	void OnWebClosed(int32 Client);
	void WebWelcome(FCubeServerPlayer& P);
	void WebSend(const FCubeServerPlayer& P, const TSharedRef<FJsonObject>& Frame);
	void WebBroadcast(const TSharedRef<FJsonObject>& Frame);
	void WebBroadcastPlayers();
	void WebBroadcastRegions();
	FCubeServerPlayer* PlayerOfWeb(int32 Client);
	int32 WebPort() const;
	FString WebAddress() const;

	// ---- what the other servers write, as the uplink or a read of the table brings it (public for the tests) ----------
	/** A player another server hosts, from their WorldPresence row. */
	static FCubeElsewhere ElsewhereOf(const TSharedPtr<FJsonObject>& Row);
	static FCubeElsewhere ElsewhereOf(const UWorldPresence* Row);
	/**
	 * Takes a pose into what is known of the players elsewhere, unless the pose known is newer: the platform sends each
	 * change on its own, so two writes of a row a moment apart can arrive the other way round.
	 */
	static ECubePoseHeard MergePose(TMap<FString, FCubeElsewhere>& Poses, const FCubeElsewhere& Pose);
	/**
	 * A presence row was deleted: it takes the player out unless the pose known of them came from another server than
	 * the one whose row went (a row left over from an older race, while the server that holds them goes on writing).
	 */
	static bool DeleteTakesOut(const FCubeElsewhere* Known, const FString& DeletedRowServer);
	static FCubeHitRecord HitOf(const TSharedPtr<FJsonObject>& Row);
	static FCubeHitRecord HitOf(const UWorldHit* Row);
	/**
	 * The bombs a read of the bomb table takes out of play here (PSV-2977): in play here, dropped more than
	 * CubeBombNoRowGraceMs ago, and with no row in any read for CubeBombRecheckMs. A bomb's rows go two minutes after it
	 * went off or fizzled (the drop function's sweep), so such a bomb is long over and this server missed its end. One read
	 * is not enough: a read of more than one page can skip a row that another write moved (PSV-3014). MissingSince comes in
	 * as when each bomb was first found with no row, and goes out as that for the bombs this read did not take out.
	 */
	static TArray<FString> BombsGoneFromTable(const TMap<FString, FCubeLiveBomb>& InPlay, const TSet<FString>& InTable, TMap<FString, int64>& MissingSince, int64 NowMs);

private:
	// ---- startup ----------------------------------------------------------------------------
	void StartServer();
	void StartOffline();
	void LoadWorld();
	void LoadBombs();
	void ApplyBombTable(const TArray<UWorldBomb*>& Rows);
	/** Reads the bomb table again: the uplink's subscription is new and heard nothing written before it. */
	void ReloadBombs();
	/** After a read of the bomb table again: the bombs in play here that the table no longer has go out of play (BombsGoneFromTable). */
	void EndBombsGoneFromTable(const TArray<UWorldBomb*>& Rows);
	/** Reads the poses of the last 5 s again, for the same reason. */
	void ReloadPresence();
	/** Reads the hits of the last 5 s again, for the same reason. */
	void ReloadHits();
	void ClaimRegion(int32 Region);
	void OpenRoom();
	void Serve();
	void RetryStartup(const FString& Why);
	FString ResolveHost() const;
	FString RoomName() const;
	/** The room type this server registers under (PLAYSERV_EXECUTOR_SLUG from hosting, else the settings'). */
	FString RoomSlug() const;
	FString Color() const;

	// ---- the loops --------------------------------------------------------------------------
	void GameTick();
	void ShareMoves();
	void TickDig(FCubeServerPlayer& Player);
	void StopDig(FCubeServerPlayer& Player);
	void TickBombs();
	/** Which server holds which region, read every 5 s as the C# servers read it (LiveRegionsAsync). */
	void ReadRegions();
	void Heartbeat();

	// ---- the uplink's data subscriptions: every write and delete of the shared tables, whoever made it ---------
	void SubscribeUplink();
	void HandleDataSubscribed(const FString& Entity);
	/** Reads a subscribed table again, a moment after its subscription went out (HandleDataSubscribed). */
	void RereadTable(const FString& Entity);
	void HandleDataUpdate(const FPlayServDataUpdate& Update);
	/** A pose of a player another server hosts: taken unless older than the one known, and a drop in health flashes them here. */
	void HearPose(const FCubeElsewhere& Pose);
	/** A hit on a player another server hosts: applied here if the victim is ours and it is fresh, then deleted. Row is set when it was read from the table. */
	void HearHit(const FCubeHitRecord& Hit, UWorldHit* Row);
	/** Deletes an applied hit heard over the uplink, by its hit id. */
	void DeleteHit(const FString& HitId, int32 Attempt = 0);
	/** Reads the whole table and takes what it says for every block that has not changed here since the read began. */
	void ReconcileCubes();
	/** A block's row is gone: the block goes back to the terrain, here and on the clients, and a row as old heard late is not applied. */
	void Bury(const FIntVector& At, int64 RowAt, const FString& By, const FString& On);
	/** A row no newer than the delete of its block, heard late over the uplink or through a read. */
	bool IsBuried(const FIntVector& At, int64 RowAt) const;
	/** A write of WriteCube ended, landed or given up. */
	void WriteDone(const FIntVector& At, bool bLanded);

	// ---- the rules --------------------------------------------------------------------------
	void Welcome(FCubeServerPlayer& Player);
	void LoadInventoryAndWelcome(const FString& Id, int32 Attempt = 0);
	/** The row could not be read when the player came: it is read again, and what they did meanwhile is added to it. */
	void ReadInventoryAgain(const FString& Id);
	/** A CubeInventory row came over the uplink: this server's own write, or another writer's to merge in. */
	void HearInventory(const FString& PlayerId, const FCubeInventory& Theirs);
	void Hurt(FCubeServerPlayer& Victim, double Damage, bool bDirected, double DX, double DY, double Strength, const FString& By);
	void Publish(const FCubeWorldUpdate& Update);
	void BroadcastCubes(const TArray<FCubeChange>& Changes, const TArray<FCubeFall>& Falls, bool bRemote);
	void WebBroadcastCubes(const TArray<FCubeChange>& Changes, const TArray<FCubeFall>& Falls, bool bRemote);
	void ShareInventory(FCubeServerPlayer& Player);
	void SendInventory(FCubeServerPlayer& Player, bool bRefused = false);
	void SendRespawn(FCubeServerPlayer& Player);
	/** A move too far for the time it took: the player is put back where their last good move left them. */
	void Correct(FCubeServerPlayer& Player, double X, double Y, double Z);
	/** This server digs, places and hands out bombs for the player (CubeServes). */
	bool InThisRegion(const FCubeServerPlayer& Player) const;
	/** The block is in this server's region or in another live server's (CubeServesBlock). */
	bool ServesBlock(int32 X, int32 Y) const;
	/** The regions other live servers hold now. */
	TArray<int32> HeldElsewhere() const;
	/** When the player stepped out of this server's region; 0 while they stand in it. */
	void NoteWhere(FCubeServerPlayer& Player);
	void RemovePlayer(const FString& Id);
	void BroadcastDig(const FString& PlayerId, int32 X, int32 Y, int32 Z, int32 Stage);
	void BroadcastHurt(const FString& PlayerId, double Health, double KX, double KY, double Strength, const FString& By);
	void BroadcastDeath(const FString& PlayerId, const FString& By);
	void BroadcastBomb(const FCubeBombRep& Frame);
	void Spawn(FCubeServerPlayer& Player);
	/** Where a player who joins stands and with what: see the definition. HelloPos is where an Unreal client's hello says it crossed. */
	void Arrive(FCubeServerPlayer& P, const FVector* HelloPos);
	TArray<FCubeHitbox> Hitboxes() const;
	TArray<TPair<FString, FCubeHitbox>> Targets() const;
	static FCubeHitbox HitboxOf(const FCubePresenceRep& Pose);
	static FCubeHitbox HitboxOf(const FCubeServerPlayer& P);
	static double EyeHeightOf(bool bSneaking) { return bSneaking ? CubeSpec::SneakEyeHeight : CubeSpec::EyeHeight; }
	FCubePresenceRep PoseOf(const FCubeServerPlayer& P) const;
	FCubeServerPlayer* PlayerOfController(AController* Controller);
	FCubeServerPlayer* PlayerById(const FString& Id);
	void PublishPlayers();

	// ---- bombs ------------------------------------------------------------------------------
	void Explode(const FCubeBombRecord& Bomb, const double* At);
	void Crater(const FCubeBombRecord& Bomb);
	void OnBomb(const FCubeBombRecord& Bomb, bool bOwned);
	void ShareBomb(const FCubeBombRecord& Bomb, bool bOwned);
	FCubeBombRecord Next(const FCubeBombRecord& Bomb, const FString& State, const FString& Holder, double X, double Y, double Z) const;
	FCubeBombRep BombFrame(const FCubeBombRecord& Bomb, const FCubeLiveBomb* Live) const;
	static FCubeBombRecord RecordOf(const UWorldBomb* Row);
	static FCubeBombRecord RecordOf(const TSharedPtr<FJsonObject>& Row);

	// ---- platform data ----------------------------------------------------------------------
	void WriteCube(const FIntVector& At, int32 Attempt = 0);
	void WritePresence(FCubeServerPlayer& Player);
	void WriteInventory(FCubeServerPlayer& Player);
	/** A player who left: their row goes CubeLeaveGraceMs later, unless another server took them over or they came back. */
	void DeletePresence(const FString& PlayerId, UWorldPresence* Row);
	/** A player who leaves this server goes on standing where it last saw them, until the next server's first pose. */
	void KeepLastPose(const FCubeServerPlayer& Player);
	void WriteHit(const FString& HitId, const FString& Victim, const FString& Attacker, double Damage, double KX, double KY, double Strength);
	void WriteRegionClaim(const TFunction<void(bool)>& Done);
	/** The room ended: the bombs over the region go up in smoke and the process exits. The region's blocks stay. */
	void FizzleBombsAndExit();
	void ExitSoon();

	// ---- the platform's rooms ---------------------------------------------------------------
	UFUNCTION() void HandleRoomEnded(const FString& InRoomName, const FString& Reason);
	UFUNCTION() void HandleRoomPlacementChanged(const FString& InRoomName, EPlayServPlacementState Placement);
	UFUNCTION() void HandlePlayerRemoved(const FString& InRoomName, const FString& PlayerId, const FString& Reason);
	void TurnAway(FCubeServerPlayer& Player, const FString& Reason);

	static int64 Now();
	void ServerLog(const FString& Text) const;

	FCubeServerWorld World;
	TMap<FString, TSharedPtr<FCubeServerPlayer>> Players;
	TMap<FString, FCubeElsewhere> Elsewhere;
	TMap<FString, FCubeLiveBomb> Bombs;
	TArray<FCubeChange> Heard;
	TSet<FString> HitsApplied;
	/** The bombs seen to go off or fizzle, and when: an older record of one (an echo, a row the sweep has not yet deleted) never
	 *  brings it back. Kept as long as the C# servers keep theirs (FinishedBombs.KeepMs). */
	TMap<FString, int64> BombsOver;
	/** Set once the bombs were read at start-up: a live bomb is known from its drop on after that. */
	bool bBombsLoaded = false;
	bool bBombsReloading = false, bPresenceReloading = false, bHitsReloading = false;
	static constexpr int64 BombsOverKeepMs = 15 * 60000;
	/** The bombs in play here that the last read of the bomb table had no row of, and since when (BombsGoneFromTable). */
	TMap<FString, int64> BombsMissingSince;
	FTimerHandle BombsRecheckTimer;
	TArray<FCubeRegionRep> Regions;
	TStrongObjectPtr<UWorldRegion> RegionRow;
	UPROPERTY() ACubeWorldState* State = nullptr;
	FString ServerName;
	int32 Region = -1;
	int64 TickCount = 0;
	int32 LocalIds = 0;
	bool bDedicated = false, bServing = false, bClosing = false;
	/** -cubeoffline: no platform at all (CubeIsOffline). */
	bool bOffline = false;
	TMap<TWeakObjectPtr<AController>, FString> OfflineIds;
	bool bRegionsBusy = false;
	TSharedPtr<class FCubeWebSocketServer> Web;
	TMap<FIntVector, FCubeTombstone> Tombstones;
	/** This server's writes of each block still on their way to the table. */
	TMap<FIntVector, int32> WritesInFlight;
	bool bReconciling = false, bReconcileAgain = false;
	/** One read of each table again after its subscription went out; another subscription meanwhile waits again. */
	TMap<FString, FTimerHandle> RereadTimers;
	/** When the WorldCube subscription first went out, when a write of this server's first landed after it, and whether any change came over it. */
	int64 CubesSubscribedAt = 0, CubeWriteLandedAt = 0;
	bool bCubeUpdatesHeard = false, bCubeUpdatesWarned = false;
	int32 RegionTry = 0;
	float Accumulator = 0;
	FTimerHandle MoveTimer, RegionTimer;
};
