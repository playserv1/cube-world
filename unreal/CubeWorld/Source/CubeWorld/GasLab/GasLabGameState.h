// The lab's game state: the server time it tells its clients is the shared clock (GasLabClock.h), so the effect start
// stamps GAS writes mean the same on every server, and a client that moves to the next server keeps its time base.
// With -gasnoclock it behaves as the stock one (each server's own world time), the control run.
// It also lays the floor on a client, which keeps it through a server switch (the floor is not replicated).
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameStateBase.h"
#include "GasLabGameState.generated.h"

UCLASS()
class CUBEWORLD_API AGasLabGameState : public AGameStateBase
{
	GENERATED_BODY()

public:
	virtual double GetServerWorldTimeSeconds() const override;
	virtual void BeginPlay() override;

protected:
	virtual void UpdateServerTimeSeconds() override;
	/**
	 * The stock client averages up to 250 samples of the server's time, so after a hitch or a jump its estimate lags
	 * by seconds, and GAS never recomputes the start times it derived from it. This one follows the latest samples,
	 * snaps when they move by more than 100 ms, and then has the local player's ability system recompute its effects.
	 */
	virtual void OnRep_ReplicatedWorldTimeSecondsDouble() override;

private:
	int32 TimeSamples = 0;
};

/** The floor: a 2 x 2 grid of 24 m regions in their colours, with the borders drawn. Spawned locally everywhere. */
UCLASS()
class CUBEWORLD_API AGasLabFloor : public AActor
{
	GENERATED_BODY()

public:
	AGasLabFloor();
	virtual void BeginPlay() override;
	static void Ensure(UWorld* World);
};
