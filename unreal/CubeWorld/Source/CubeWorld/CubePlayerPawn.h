// The player: a first-person camera over the Minecraft body simulated in CubePhysics at 20 ticks a
// second, the keys and mouse, aiming, digging, placing and hitting, bombs picked up and thrown, the tombstones
// of the dead. Movement is the client's, as Minecraft's is, and reported to the server every tick; everything
// else is a request the server judges. The pawn is replicated to its owner only: other players are drawn from
// the presence list the server replicates for everyone (CubeWorldState).
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "CubePhysics.h"
#include "CubeVoxelWorld.h"
#include "CubeWorldState.h"
#include "CubePlayerPawn.generated.h"

class UCameraComponent;
class UCubeWorldGameInstance;
class ACubeAvatar;
class ACubeWorldActor;
class ACubeBomb;
class ACubeTombstone;
struct FCubeBombFrame;
struct FCubePose;
struct FCubePresence;

struct FCubeAim
{
	bool bBlock = false, bPlayer = false;
	FCubeRayHit Hit;
	FString PlayerId;
};

/** The welcome: which server this is and where the player stands. */
USTRUCT()
struct FCubeWelcomeRep
{
	GENERATED_BODY()
	UPROPERTY() FString Server;
	UPROPERTY() FString Color;
	UPROPERTY() FString Room;
	UPROPERTY() int32 Region = -1;
	UPROPERTY() float X = 0;
	UPROPERTY() float Y = 0;
	UPROPERTY() float Z = 0;
	UPROPERTY() float Health = 20;
	/** How many world chunks follow. */
	UPROPERTY() int32 Chunks = 0;
};

UCLASS()
class CUBEWORLD_API ACubePlayerPawn : public APawn
{
	GENERATED_BODY()

public:
	ACubePlayerPawn();
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	virtual void Tick(float DeltaSeconds) override;
	/** Binds to the game at once, for a pawn the client spawns itself and that must hear the very next frame. */
	void BindNow() { Bind(); }
	virtual void SetupPlayerInputComponent(UInputComponent* Input) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	FCubeBody Body;
	FCubeAim Aim;
	double HurtUntil = 0;
	bool bMouseCaptured = false;

	/** The PlayServ player id the server admitted this connection as; set by the server, read by its owner. */
	UPROPERTY(ReplicatedUsing=OnRep_PlayerId) FString PlayerId;

	/** The aimed block, if any, for the HUD and the outline. */
	const FCubeAim& CurrentAim() const { return Aim; }

	// ---- to the server ----------------------------------------------------------------------
	/** Who this is, and where they were when they crossed a border (bCross), else the server's spawn. */
	UFUNCTION(Server, Reliable) void ServerHello(const FString& Name, bool bCross, float X, float Y, float Z);
	UFUNCTION(Server, Unreliable) void ServerMove(float X, float Y, float Z, float Yaw, float Pitch, bool bOnGround, bool bSneaking, bool bSprinting);
	UFUNCTION(Server, Reliable) void ServerDig(int32 X, int32 Y, int32 Z, bool bStart);
	UFUNCTION(Server, Reliable) void ServerPlace(int32 X, int32 Y, int32 Z, int32 NX, int32 NY, int32 NZ, uint8 Kind);
	UFUNCTION(Server, Reliable) void ServerAttack(const FString& Target);
	UFUNCTION(Server, Reliable) void ServerRespawn();
	UFUNCTION(Server, Reliable) void ServerThrow(float DX, float DY, float DZ);

	// ---- the same requests, by whichever door the server is behind: an RPC on an Unreal server, a JSON frame on a C# one
	void CmdMove(double X, double Y, double Z, double Yaw, double Pitch, bool bOnGround, bool bSneaking, bool bSprinting);
	void CmdDig(int32 X, int32 Y, int32 Z, bool bStart);
	void CmdPlace(int32 X, int32 Y, int32 Z, int32 NX, int32 NY, int32 NZ, FName Kind);
	void CmdAttack(const FString& Target);
	void CmdRespawn();
	void CmdThrow(const FVector& Direction);

	// ---- from the server --------------------------------------------------------------------
	UFUNCTION(Client, Reliable) void ClientWelcome(const FCubeWelcomeRep& Welcome, const TArray<FCubeStackRep>& Stacks);
	UFUNCTION(Client, Reliable) void ClientWorldChunk(const TArray<FCubeCellRep>& Cells, bool bLast);
	UFUNCTION(Client, Reliable) void ClientBombs(const TArray<FCubeBombRep>& InBombs);
	UFUNCTION(Client, Reliable) void ClientInventory(const TArray<FCubeStackRep>& Stacks);
	UFUNCTION(Client, Reliable) void ClientRespawn(float X, float Y, float Z);
	/** An operator closed the room or removed this player; the connection closes right after. */
	UFUNCTION(Client, Reliable) void ClientTurnedAway(const FString& Reason);

private:
	UFUNCTION() void OnRep_PlayerId();
	void Bind();
	void GameTick();
	void DigTick();
	void UpdateAim();
	void SendMove(double Yaw, double Pitch);
	void HandleWelcome(const FCubePose& You, bool bTeleport);
	void HandleRespawn(const FCubePose& You);
	void HandleHurt(const FString& PlayerId, double Health, double KX, double KY, double Strength);
	void HandleDeath(const FString& PlayerId, const FString& By);
	void HandlePlayers(const TArray<FCubePresence>& Players);
	void HandleCube(int32 X, int32 Y, int32 Z, FName Kind);
	void HandleBomb(const FCubeBombFrame& Frame);
	void RemoveBomb(const FString& Id);
	void TickBomb(ACubeBomb* Bomb);
	void PlaceHeld(ACubeBomb* Bomb);
	void ThrowBomb();
	void UpdateHolding();
	void ShowMyTomb();
	void Spawn(double X, double Y, double Z);
	void Unstick();
	/** The -walkto test sprints by itself (the keys read from the keyboard would say it does not). */
	bool bTestSprint = false;
	void CaptureMouse(bool bCapture);
	void ClickMenu();
	void SetupUnattended();
	ACubeWorldActor* WorldActor() const;

	// input
	void OnMoveForward(float V) { AxisForward = V; }
	void OnMoveRight(float V) { AxisRight = V; }
	void OnTurn(float V);
	void OnLookUp(float V);
	void OnJump(bool b) { bJumpHeld = b; }
	void OnSprint(bool b) { bSprintHeld = b; }
	void OnSneak(bool b) { bSneakHeld = b; }
	void OnDig(bool b);
	void OnPlace();
	void OnSlot(int32 Index);
	void OnSlotNext() { OnSlot((Slot() + 1) % 9); }
	void OnSlotPrevious() { OnSlot((Slot() + 8) % 9); }
	void OnConfirm();
	void OnRelease();
	int32 Slot() const;

	UPROPERTY() UCameraComponent* Camera = nullptr;
	UPROPERTY() TMap<FString, ACubeAvatar*> Avatars;
	UPROPERTY() UMaterialInterface* SkinMaterial = nullptr;
	UPROPERTY() TMap<FString, ACubeBomb*> Bombs;
	UPROPERTY() ACubeTombstone* MyTomb = nullptr;
	int64 TickCount = 0;
	UCubeWorldGameInstance* Game = nullptr;
	float AxisForward = 0, AxisRight = 0;
	float TestForward = 0;   // -selftest drives the body without a keyboard
	bool bJumpHeld = false, bSprintHeld = false, bSneakHeld = false, bDigHeld = false;
	float Accumulator = 0;
	int32 DigCooldown = 0;
	bool bDigging = false;
	FIntVector DigTarget;
	FString LastPose;
	FIntVector TestPlaced, TestDug;
	TOptional<float> TestWalkTo, TestWalkToY;
	float Fov = 70.f;
	bool bBound = false;
};
