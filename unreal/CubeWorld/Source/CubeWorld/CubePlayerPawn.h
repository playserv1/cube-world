// The player: a first-person camera over the Minecraft body simulated in CubePhysics at 20 ticks a
// second, the keys and mouse, aiming, digging, placing and hitting, bombs picked up and thrown, the tombstones
// of the dead, and the reports to the server.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "CubePhysics.h"
#include "CubeVoxelWorld.h"
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

UCLASS()
class CUBEWORLD_API ACubePlayerPawn : public APawn
{
	GENERATED_BODY()

public:
	ACubePlayerPawn();
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void SetupPlayerInputComponent(UInputComponent* Input) override;

	FCubeBody Body;
	FCubeAim Aim;
	double HurtUntil = 0;
	bool bMouseCaptured = false;

	/** The aimed block, if any, for the HUD and the outline. */
	const FCubeAim& CurrentAim() const { return Aim; }

private:
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
	void CaptureMouse(bool bCapture);
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
	float Fov = 70.f;
};
