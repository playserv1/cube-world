// The client's session: sign-in through the PlayServ SDK, the room list, the join ticket and the travel to the
// dedicated server the ticket names; then a border crossing to the next server with the position kept. What the
// server replicates (CubeWorldState) and sends the player (CubePlayerPawn's client RPCs) lands here and goes to
// the world, the pawn and the HUD through the delegates below.
#pragma once

#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "CubeVoxelWorld.h"
#include "CubeTextures.h"
#include "CubeBombs.h"
#include "CubeWorldState.h"
#include "Engine/EngineTypes.h"
#include "CubeWorldGameInstance.generated.h"

class FCubeSocket;
class FJsonObject;

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

	/** True once this server's welcome arrived and until the connection goes. */
	bool IsConnected() const { return bWelcomed; }
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
	/** The bomb in the player's hand, if any: right click throws it instead of placing a block. */
	FString Holding;
	FString Status = TEXT("Press Enter to play");
	TArray<FString> LogLines;
	TArray<FCubePresence> Players;
	/** Where the body is, written by the pawn every tick; the position a crossing keeps. */
	FCubeCrossing LastBody;
	/** The position to take on the next server; set for a crossing, unset for a fresh join. */
	FCubeCrossing Crossing;

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
	void Browse();
	/** Every room type answered: pick a room and enter it. */
	void Browsed();
	void Reconnect();
	void TravelToUnrealServer(const FString& RoomName, const FString& Url, bool bTeleport);
	void ConnectSocket(const FString& RoomName, const FString& Host, int32 Port, bool bSecure, const FString& ReservationToken, bool bTeleport);
	void OnSocketFrame(const TSharedPtr<FJsonObject>& Frame, const FString& RoomName, bool bTeleport);
	void OnSocketWelcome(const TSharedPtr<FJsonObject>& Frame, const FString& RoomName, bool bTeleport);
	void CloseSockets();
	bool IsInNetworkedWorld() const;
	void HandleNetworkFailure(UWorld* World, UNetDriver* NetDriver, ENetworkFailure::Type FailureType, const FString& ErrorString);
	void HandlePostLoadMap(UWorld* LoadedWorld);
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
	/** The room travelled to, until its welcome names it. */
	FString Travelling;
	/** A crossing that fails is tried again three seconds later, not on every tick. */
	double CrossAfter = 0;
	/** A room that turned the player away (an operator closed it, or removed them) is not tried again before this time. */
	TMap<FString, double> NotBefore;
	TArray<FString> Candidates;
	/** The room type each known room is registered under (the C# servers' or the Unreal servers'), from the browse and the regions. */
	TMap<FString, FString> RoomSlugs;
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
