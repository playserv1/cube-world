// A lab player's controller. Keys for a person (WASD, mouse, left button throws, space jumps); for an unattended
// client an autopilot (-gaswalk=x,y;x,y walks the waypoints, -gasthrowevery=<s> throws ahead, -gaswalkat=<s> starts
// walking that long after joining). It logs what the client sees of its own GAS state ten times a second, on the
// shared clock, so a crossing can be checked from the client's side: the cooldown and the burning keep counting down
// through the server switch.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "AIController.h"
#include "GasLabPlayerController.generated.h"

UCLASS()
class CUBEWORLD_API AGasLabPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	AGasLabPlayerController();

	/** The server hands this player to the server of ToRegion: travel there, keeping the world. */
	UFUNCTION(Client, Reliable)
	void ClientGasCross(const FString& Address, const FString& Id, int32 ToRegion);
	/** Scenario: walk to To (the autopilot keeps walking through a server switch). */
	UFUNCTION(Client, Reliable)
	void ClientGasWalk(FVector To);
	/** Scenario: throw now, from the client, as a player pressing the button (a predicted activation). */
	UFUNCTION(Client, Reliable)
	void ClientGasThrow();

	virtual void PlayerTick(float DeltaTime) override;
	virtual void SetupInputComponent() override;

	/** Set by the game mode from the travel options. */
	FString GasId, GasName;
	bool bGasCrossing = false;

private:
	void Throw();
	void Autopilot(float DeltaTime);
	void LogState();

	double NextLog = 0;
	FVector2D Look = FVector2D::ZeroVector;
};

/** A server-side bot: walks to a point, throws when told. Its player state carries the ability system as a player's does. */
UCLASS()
class CUBEWORLD_API AGasLabBotController : public AAIController
{
	GENERATED_BODY()

public:
	AGasLabBotController();
	virtual void Tick(float DeltaSeconds) override;

	TOptional<FVector> WalkTo;
};
