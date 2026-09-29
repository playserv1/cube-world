// The session: sign-in through the PlayServ SDK, the room list and the join ticket, and the game socket
// to the machine the ticket names, speaking the same JSON frames as the browser client. Frames are
// handed to the world, the pawn and the HUD through the delegates below.
#pragma once

#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "CubeVoxelWorld.h"
#include "CubeTextures.h"
#include "CubeBombs.h"
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
	FString Room, Color, Server;
};

struct FCubePose
{
	double X = 0, Y = 0, Z = 0, Health = 20;
};

DECLARE_MULTICAST_DELEGATE_TwoParams(FCubeOnWelcome, const FCubePose& /*You*/, bool /*bTeleport*/);
DECLARE_MULTICAST_DELEGATE_FourParams(FCubeOnCube, int32, int32, int32, FName /*Kind, None = generated*/);
/** After one "cube" frame or one batched "cubes" frame, every block it changed: rebuild each chunk once. */
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

	// ---- the session ------------------------------------------------------------------------
	/** Sign in as a guest with this name, list the rooms and enter the first one. */
	void StartPlay(const FString& Name);
	/** Join a room by name; with bTeleport false the player keeps their position (a border crossing). */
	void Enter(const FString& RoomName, bool bTeleport = true);
	/** Called by the pawn every tick with its x: crosses into the region's server when the border is passed. */
	void MaybeCross(double X);
	void Send(const TSharedRef<FJsonObject>& Frame);
	void Log(const FString& Text);

	bool IsConnected() const;
	/** "you", or the name another player goes by. */
	FString NameOf(const FString& Id) const;
	bool IsSigningIn() const { return bSigningIn; }

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
	void Reconnect();
	void Connect(const FString& RoomName, const FString& Host, int32 Port, const FString& Path, const FString& ReservationToken, bool bTeleport);
	void OnFrame(const TSharedPtr<FJsonObject>& Frame, bool bTeleport);
	void ReadInventory(const TSharedPtr<FJsonObject>& Object);
	void ReadCube(const TSharedPtr<FJsonObject>& Cube, bool bDelete, TArray<FIntVector>& Changed);
	void ReadFall(const TSharedPtr<FJsonObject>& Fall);
	FString RoomOfRegion(int32 InRegion) const;

	TSharedPtr<FCubeSocket> Socket;
	TSharedPtr<FCubeSocket> Pending;
	bool bSigningIn = false;
	bool bSwitching = false;
	/** A crossing that fails is tried again three seconds later, not on every tick. */
	double CrossAfter = 0;
	/** A room that turned the player away (an operator closed it, or removed them) is not tried again before this time. */
	TMap<FString, double> NotBefore;
	/** Whether RoomName may be tried now. */
	bool MayTry(const FString& RoomName) const { return FPlatformTime::Seconds() >= NotBefore.FindRef(RoomName); }
	/** Notes an operator's close or removal of RoomName (from the socket's close reason or the join's problem code); returns the message for the player, empty for anything else. */
	FString TurnedAway(const FString& RoomName, const FString& ReasonOrCode);
	TArray<FString> Candidates;
	FTimerHandle RetryTimer;
};
