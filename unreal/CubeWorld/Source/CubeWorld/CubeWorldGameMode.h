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
#include "CubeWorldGameMode.generated.h"

class ACubePlayerPawn;
struct FPlayServError;

/** A WorldBomb row as the server follows it. */
struct FCubeBombRecord
{
	FString Id, State, Holder;
	double X = 0, Y = 0, Z = 0, VX = 0, VY = 0, VZ = 0;
	int64 DroppedAt = 0, At = 0;

	static int32 Rank(const FString& State) { return State == TEXT("free") ? 0 : State == TEXT("held") ? 1 : State == TEXT("flying") ? 2 : 3; }
	bool IsOver() const { return Rank(State) == 3; }
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
	FString Id, Name;
	double X = 0, Y = 0, Z = 0, Yaw = 0, Pitch = 0, Health = 20;
	bool bSneaking = false, bSprinting = false;
	bool bMoved = false, bDead = false, bAirborne = false, bWelcomed = false;
	double Peak = 0;
	int64 LastAttackTick = -1000000, LastHurtTick = -1000000;
	TOptional<FCubeDig> Dig;
	/** The bomb in the player's hand. A player holds one at a time and can only throw it. */
	FString Bomb;
	FCubeInventory Inventory;
	TStrongObjectPtr<UCubeInventory> InventoryRow;
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
	virtual void Logout(AController* Exiting) override;

	/** True in a dedicated server that has claimed its region and opened its room. */
	bool IsServing() const { return bServing; }

	// ---- what the players ask (from the pawn's server RPCs) ---------------------------------
	void OnHello(ACubePlayerPawn* Pawn, const FString& Name, bool bCross, double X, double Y, double Z);
	void OnMove(ACubePlayerPawn* Pawn, double X, double Y, double Z, double Yaw, double Pitch, bool bOnGround, bool bSneaking, bool bSprinting);
	void OnDig(ACubePlayerPawn* Pawn, int32 X, int32 Y, int32 Z, bool bStart);
	void OnPlace(ACubePlayerPawn* Pawn, int32 X, int32 Y, int32 Z, int32 NX, int32 NY, int32 NZ, FName Kind);
	void OnAttack(ACubePlayerPawn* Pawn, const FString& Target);
	void OnRespawn(ACubePlayerPawn* Pawn);
	void OnThrow(ACubePlayerPawn* Pawn, double DX, double DY, double DZ);

private:
	// ---- startup ----------------------------------------------------------------------------
	void StartServer();
	void LoadWorld();
	void LoadBombs();
	void ClaimRegion(int32 Region);
	void OpenRoom();
	void Serve();
	void RetryStartup(const FString& Why);
	FString ResolveHost() const;
	FString RoomName() const;
	FString Color() const;

	// ---- the loops --------------------------------------------------------------------------
	void GameTick();
	void ShareMoves();
	void TickDig(FCubeServerPlayer& Player);
	void StopDig(FCubeServerPlayer& Player);
	void TickBombs();
	void PollCubes();
	void PollPresence();
	void PollHits();
	void PollBombs();
	void PollRegions();
	void Heartbeat();

	// ---- the rules --------------------------------------------------------------------------
	void Welcome(FCubeServerPlayer& Player);
	void Hurt(FCubeServerPlayer& Victim, double Damage, bool bDirected, double DX, double DY, double Strength, const FString& By);
	void Publish(const FCubeWorldUpdate& Update);
	void BroadcastCubes(const TArray<FCubeChange>& Changes, const TArray<FCubeFall>& Falls, bool bRemote);
	void ShareInventory(FCubeServerPlayer& Player);
	void Spawn(FCubeServerPlayer& Player);
	TArray<FCubeHitbox> Hitboxes() const;
	TArray<TPair<FString, FCubeHitbox>> Targets() const;
	static FCubeHitbox HitboxOf(const FCubePresenceRep& Pose);
	static FCubeHitbox HitboxOf(const FCubeServerPlayer& P);
	static double EyeHeightOf(bool bSneaking) { return bSneaking ? CubeSpec::SneakEyeHeight : CubeSpec::EyeHeight; }
	FCubePresenceRep PoseOf(const FCubeServerPlayer& P) const;
	FCubeServerPlayer* PlayerOf(ACubePlayerPawn* Pawn);
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

	// ---- platform data ----------------------------------------------------------------------
	void WriteCube(const FIntVector& At, int32 Attempt = 0);
	void WritePresence(FCubeServerPlayer& Player);
	void WriteInventory(FCubeServerPlayer& Player);
	void DeletePresence(const FString& PlayerId, UWorldPresence* Row);
	void WriteHit(const FString& HitId, const FString& Victim, const FString& Attacker, double Damage, double KX, double KY, double Strength);
	void WriteRegionClaim(const TFunction<void(bool)>& Done);
	void ClearRegionAndExit();
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
	TArray<FCubeRegionRep> Regions;
	TStrongObjectPtr<UWorldRegion> RegionRow;
	UPROPERTY() ACubeWorldState* State = nullptr;
	FString ServerName;
	int32 Region = -1;
	int64 TickCount = 0;
	int64 LastCubeAt = 0, LastBombAt = 0;
	int32 LocalIds = 0;
	bool bDedicated = false, bServing = false, bClosing = false;
	bool bCubesBusy = false, bPresenceBusy = false, bHitsBusy = false, bBombsBusy = false, bRegionsBusy = false;
	int32 RegionTry = 0;
	float Accumulator = 0;
	FTimerHandle MoveTimer, CubeTimer, PresenceTimer, HitTimer, BombTimer, RegionTimer;
};
