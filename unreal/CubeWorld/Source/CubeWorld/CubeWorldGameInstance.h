// The client's session: sign-in through the PlayServ SDK, the room list, the join ticket and the travel to the
// dedicated server the ticket names; then a border crossing to the next server with the position kept. What the
// server replicates (CubeWorldState) and sends the player (CubePlayerPawn's client RPCs) lands here and goes to
// the world, the pawn and the HUD through the delegates below.
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Engine/GameInstance.h"
#include "CubeVoxelWorld.h"
#include "CubePhysics.h"
#include "CubeTextures.h"
#include "CubeBombs.h"
#include "CubeWorldState.h"
#include "Engine/EngineTypes.h"
#include "CubeWorldGameInstance.generated.h"

class FCubeSocket;
struct FCubeSocketFrame;
class FJsonObject;
class ACubeAvatar;
class ACubeBomb;
class ACubeTombstone;

struct FCubePresence
{
	FString Id, Name, Server, Color;
	double X = 0, Y = 0, Z = 0, Yaw = 0, Pitch = 0, Health = 20;
	bool bSneaking = false, bSprinting = false;
};

struct FCubeRegion
{
	int32 Region = -1;
	FString Room, Color, Server, Slug;
};

struct FCubePose
{
	double X = 0, Y = 0, Z = 0, Health = 20;
};

/** Where the player was when they walked over a border: the next server places them there instead of at its spawn. */
struct FCubeCrossing
{
	bool bSet = false;
	double X = 0, Y = 0, Z = 0, Yaw = 0, Pitch = 0;
	/** The body's speed, so a walk over a border goes on at the same pace. */
	double VX = 0, VY = 0, VZ = 0;
	bool bSprinting = false, bSneaking = false;
	/** The movement input at the border, kept until the next controller's own input arrives. */
	float Forward = 0, Strafe = 0;
};

DECLARE_MULTICAST_DELEGATE_TwoParams(FCubeOnWelcome, const FCubePose& /*You*/, bool /*bTeleport*/);
DECLARE_MULTICAST_DELEGATE_FourParams(FCubeOnCube, int32, int32, int32, FName /*Kind, None = generated*/);
/** After one batch of changes, every block it changed: rebuild each chunk once. */
DECLARE_MULTICAST_DELEGATE_OneParam(FCubeOnCubes, const TArray<FIntVector>&);
DECLARE_MULTICAST_DELEGATE_FiveParams(FCubeOnFall, FName /*Kind*/, int32 /*X*/, int32 /*Y*/, int32 /*FromZ*/, int32 /*ToZ*/);
DECLARE_MULTICAST_DELEGATE_FiveParams(FCubeOnDig, const FString& /*PlayerId*/, int32, int32, int32, int32 /*Stage, -1 clears*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FCubeOnPlayers, const TArray<FCubePresence>&);
DECLARE_MULTICAST_DELEGATE_FiveParams(FCubeOnHurt, const FString& /*PlayerId*/, double /*Health*/, double /*KX*/, double /*KY*/, double /*Strength*/);
DECLARE_MULTICAST_DELEGATE_TwoParams(FCubeOnDeath, const FString& /*PlayerId*/, const FString& /*By*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FCubeOnRespawn, const FCubePose&);
DECLARE_MULTICAST_DELEGATE(FCubeOnInventory);
DECLARE_MULTICAST_DELEGATE_OneParam(FCubeOnBomb, const FCubeBombFrame&);

UCLASS()
class CUBEWORLD_API UCubeWorldGameInstance : public UGameInstance
{
	GENERATED_BODY()

public:
	virtual void Init() override;
	virtual void Shutdown() override;
	virtual void ReturnToMainMenu() override;

	// ---- the session ------------------------------------------------------------------------
	/** Sign in as a guest with this name, list the rooms and enter the first one. */
	void StartPlay(const FString& Name);
	/** Join a room by name and travel to its server; with bTeleport false the player keeps their position (a border crossing). */
	void Enter(const FString& RoomName, bool bTeleport = true);
	/** Called by the pawn every tick with its x and y: crosses into the region's server when a border is passed. */
	void MaybeCross(double X, double Y);
	void Log(const FString& Text);
	/** The pawn of the next server is placed: the view is its own again. */
	void EndCrossingView();

	/** True once this server's welcome arrived and until the connection goes. */
	bool IsConnected() const { return bWelcomed; }
	/** In the game for the player: connected, or walking over a border into the next server. The HUD shows through a crossing. */
	bool IsInPlay() const { return bWelcomed || Crossing.bSet; }
	/** The field of view the pawn draws with (horizontal degrees), for the view held through a crossing. */
	float LastHorizontalFov = 0, LastFov = 0;
	/** True while the server is a C# one, reached over the JSON socket; false on an Unreal server, reached over Iris. */
	bool IsViaSocket() const { return bViaSocket; }
	/** A JSON frame to the C# server (nothing while on an Unreal server). */
	void Send(const TSharedRef<FJsonObject>& Frame);
	/** "you", or the name another player goes by. */
	FString NameOf(const FString& Id) const;
	bool IsSigningIn() const { return bSigningIn; }

	// ---- what the server tells this client (called from the pawn's client RPCs and the state actor) ----
	void OnWelcomed(const FString& InServer, const FString& InColor, const FString& InRoom, int32 InRegion, const FCubePose& You, const TArray<FCubeStackRep>& Stacks, int32 ChunkCount);
	void OnWorldChunk(const TArray<FCubeCellRep>& Cells, bool bLast);
	void ApplyCubes(const TArray<FCubeChangeRep>& Changes, const TArray<FCubeFallRep>& Falls, bool bRemote);
	void SetInventory(const TArray<FCubeStackRep>& Stacks);
	/** Reads a socket frame on the worker that parsed it: a welcome's blocks into a cell map, then out of the frame's JSON. */
	static void DecodeSocketFrame(FCubeSocketFrame& Parsed);
	void SetPlayers(const TArray<FCubePresenceRep>& InPlayers);
	void SetRegions(const TArray<FCubeRegionRep>& InRegions);
	void OnHurtFrame(const FString& PlayerId, double InHealth, double KX, double KY, double Strength);
	void OnDeathFrame(const FString& PlayerId, const FString& By);
	void OnRespawnFrame(const FCubePose& You);
	void OnBombFrame(const FCubeBombRep& Bomb);
	/** The server turned this player away (an operator's close or removal): note it and say so. */
	void TurnedAwayBy(const FString& Reason);

	// ---- state the pawn and HUD read --------------------------------------------------------
	FString PlayerName, PlayerId;
	FString Room, Server, Color = TEXT("grey");
	int32 Region = -1;
	TArray<FCubeRegion> Regions;
	TArray<FName> Hotbar;
	TMap<FName, int32> Inventory;
	int32 Slot = 0;
	double Health = 20;
	bool bDead = false;
	bool bPlaced = false;
	/** Set once a border has been crossed in this run (the -holdkeys test walks by itself only up to the first one). */
	bool bCrossedOnce = false;
	/** The -walkto spot the test walks to now, kept through crossings (each server's pawn is a new one). */
	int32 WalkSpot = 0;
	/** The bomb in the player's hand, if any: right click throws it instead of placing a block. */
	FString Holding;
	FString Status = TEXT("Press Enter to play");
	TArray<FString> LogLines;
	TArray<FCubePresence> Players;
	/** Where the body is, written by the pawn every tick; the position a crossing keeps. */
	FCubeCrossing LastBody;
	/** The position to take on the next server; set for a crossing, unset for a fresh join. */
	FCubeCrossing Crossing;

	// ---- the body between two servers ------------------------------------------------------------
	// From the moment the old world goes until the next server's pawn is placed (0.2 to 0.8 s, the next server's round
	// trips) there is no pawn: the client carries the body itself, with the same physics, the keys and the mouse, and the
	// view follows it, so the player never stops at a border. The pawn takes the body over where it has got to.
	/** The old pawn's whole body, its physics remainder and when it was stepped last. */
	FCubeBody LastFullBody;
	double LastAccumulator = 0, LastBodyTime = 0;
	FCubeBody GapBody;
	double GapAccumulator = 0, GapLastTime = 0;
	float GapYawDeg = 0, GapPitchDeg = 0, GapFov = 0;
	bool bGapActive = false, bGapTestWalk = false;
	/** Steps the carried body up to now and aims the view at its eyes. */
	void StepGap();

	// ---- the client's own actors, from one pawn to the next across a crossing that keeps the world (UCubeGameEngine) ----
	void CarryOver(TMap<FString, ACubeAvatar*>& InAvatars, TMap<FString, ACubeBomb*>& InBombs, ACubeTombstone*& InTomb);
	void TakeCarried(TMap<FString, ACubeAvatar*>& OutAvatars, TMap<FString, ACubeBomb*>& OutBombs, ACubeTombstone*& OutTomb);
	/** What no pawn took: destroyed (a fresh join starts from the next server's lists). */
	void DropCarried();

	FCubeVoxelWorld World;
	FCubeTextures Textures;

	// ---- frames -----------------------------------------------------------------------------
	FCubeOnWelcome OnWelcome;
	FCubeOnCube OnCube;
	FCubeOnFall OnFall;
	FCubeOnDig OnDig;
	FCubeOnPlayers OnPlayers;
	FCubeOnHurt OnHurt;
	FCubeOnDeath OnDeath;
	FCubeOnRespawn OnRespawn;
	FCubeOnInventory OnInventory;
	FCubeOnCubes OnCubes;
	FCubeOnBomb OnBomb;

private:
	/** A guest is kept per name on this machine (Saved/Guests), as the browser keeps one per name: signing in again under
	 * the same name resumes the same player instead of making a new one every launch. */
	void SignInAsNewGuest();
	void KeepGuest(const FString& RefreshToken);
	UFUNCTION() void HandleSessionLost();
	void Browse();
	/** Every room type answered: pick a room and enter it. */
	void Browsed();
	void Reconnect();
	void TravelToUnrealServer(const FString& RoomName, const FString& Url, bool bTeleport);
	void ConnectSocket(const FString& RoomName, const FString& Host, int32 Port, bool bSecure, const FString& ReservationToken, bool bTeleport);
	void OnSocketFrame(const TSharedPtr<FJsonObject>& Frame, const FString& RoomName, bool bTeleport);
	/** PreRead: the welcome's blocks, read where the frame was parsed (FCubeSocket::Decode). */
	void OnSocketWelcome(const TSharedPtr<FJsonObject>& Frame, const FString& RoomName, bool bTeleport, TOptional<TMap<FIntVector, FName>> PreRead = {});
	void CloseSockets();
	bool IsInNetworkedWorld() const;
	void HandleNetworkFailure(UWorld* World, UNetDriver* NetDriver, ENetworkFailure::Type FailureType, const FString& ErrorString);
	void HandlePostLoadMap(UWorld* LoadedWorld);
	/** From the moment the old server's pawn is gone until the next one's is placed: the view held at the player's eyes,
	 * the body carried by the client (both crossings, the one that reloads the map and the one that keeps it). */
	void BeginCrossingGap(UWorld* InWorld);
	/** UCubeGameEngine: the next Unreal server's connection took over this world (bConnected), or it has no server now. */
	void HandleServerSwitched(UWorld* InWorld, bool bConnected);
	void HandleSeamlessTravelFailed(const FString& Why);
	/** Leaves the Unreal server for the C# one whose welcome just came over the socket, keeping the world. */
	void LeaveUnrealServerKeepWorld();
	/** True between a seamless ClientTravel and the world changing hands (or the handshake failing). */
	bool bSeamlessCrossing = false;
	TMap<FString, TWeakObjectPtr<ACubeAvatar>> CarriedAvatars;
	TMap<FString, TWeakObjectPtr<ACubeBomb>> CarriedBombs;
	TWeakObjectPtr<ACubeTombstone> CarriedTomb;
	FString RoomOfRegion(int32 InRegion) const;
	/** Notes an operator's close or removal of RoomName; returns the message for the player, empty for anything else. */
	FString TurnedAway(const FString& RoomName, const FString& ReasonOrCode);
	/** Whether RoomName may be tried now. */
	bool MayTry(const FString& RoomName) const { return FPlatformTime::Seconds() >= NotBefore.FindRef(RoomName); }
	void Disconnected(const FString& Why);

	bool bSigningIn = false;
	bool bSwitching = false;
	bool bViaSocket = false;
	TSharedPtr<FCubeSocket> Socket;
	TSharedPtr<FCubeSocket> PendingSocket;
	/** A socket connection that waits for the client to leave an Unreal server's world first. */
	struct FSocketPlan { bool bSet = false; FString RoomName, Host, ReservationToken; int32 Port = 0; bool bSecure = false, bTeleport = true; };
	FSocketPlan SocketPlan;
	bool bWelcomed = false;
	bool bWorldLoaded = false;
	/** After a crossing the world the client holds stays; the next server's snapshot is gathered here and only its differences
	 * are applied, so nothing on screen is torn down. */
	/** Holds the view where the player stands while the next server's pawn is on its way after a crossing. */
	TWeakObjectPtr<class ACameraActor> CrossingCamera;
	FDelegateHandle CrossingViewTicker;
	double CrossingViewUntil = 0;
	int32 CrossingBlankFrames = 0, CrossingWrongFrames = 0;
	void HoldCrossingView(UWorld* InWorld, ELevelTick TickType, float DeltaSeconds);
	/** Puts the placeholder controller, and so the view, at the player's eyes. */
	void AimPlaceholder(class APlayerController* PC);
	void AimCrossingViewBeforeDraw();
	FDelegateHandle CrossingDrawHandle;
	FVector CrossingEye = FVector::ZeroVector;
	FRotator CrossingLook = FRotator::ZeroRotator;
	/** -logcrossing: what every frame around a crossing is drawn from, for finding a wrong frame. */
	void LogFramesFor(double Seconds);
	void LogDrawnFrame();
	void LogEndOfFrame();
	FDelegateHandle EndFrameLogHandle;
	uint64 LastDrawnFrame = 0;
	FDelegateHandle DrawLogHandle;
	double DrawLogUntil = 0;
	bool bSnapshotDiff = false;
	TMap<FIntVector, FName> Snapshot;
	void ApplySnapshot();
	/** The room travelled to, until its welcome names it. */
	FString Travelling;
	/** A crossing that fails is tried again three seconds later, not on every tick. */
	double CrossAfter = 0;
	/** A room that turned the player away (an operator closed it, or removed them) is not tried again before this time. */
	TMap<FString, double> NotBefore;
	TArray<FString> Candidates;
	/** The room type each known room is registered under (the C# servers' or the Unreal servers'), from the browse and the regions. */
	TMap<FString, FString> RoomSlugs;
	/** Rooms already retried under the other room type (once each). */
	TSet<FString> RetriedOtherType;
	/** Browses still to answer, when the room types are listed together. */
	int32 BrowsesPending = 0;
	TArray<FString> BrowseFound;
	FString BrowseError;
	FCubePose WelcomePose;
	int32 ChunksExpected = 0, ChunksReceived = 0;
	/** Changes heard before the world snapshot finished arriving; applied after it. */
	struct FPendingCubes { TArray<FCubeChangeRep> Changes; TArray<FCubeFallRep> Falls; bool bRemote; };
	TArray<FPendingCubes> PendingCubes;
	FTimerHandle RetryTimer;
};
