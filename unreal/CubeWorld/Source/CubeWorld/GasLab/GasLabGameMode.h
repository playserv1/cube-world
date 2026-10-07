// The lab's game mode, picked with ?game=/Script/CubeWorld.GasLabGameMode on a server started with -gaslab.
// Players join with ?gasname=<name>; a player handed over by a neighbour comes with ?gasplayer=<id>?gascross=1 and is
// spawned only once its state has arrived (or after 5 s, fresh). Abilities are granted on spawn from the same set on
// every server, as Lyra's ability sets are, so a handoff carries effects and attributes, not abilities.
//
// -gasscenario= runs the research cases with server-side bots (all times from the server's start):
//   corner  four servers; two players stand near the corner where the four regions meet, on each server; a thrower on
//           region 0 throws a creeper at the corner at 6, 14 and 22 s.
//   cross   two servers (regions 0 and 1); the thrower burns a walker near the border at 5 and 9 s, the walker walks
//           over to region 1 at 10 s, the thrower follows at 9.2 s with its cooldown running, and throws again
//           from region 1 at 14 s.
//   soak    -gasbots=<n> bots per server walk between random points of the whole world and throw at each other.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "GasLabGameMode.generated.h"

class AGasLabCharacter;
struct FGasLabWirePlayer;

UCLASS()
class CUBEWORLD_API AGasLabGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AGasLabGameMode();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual FString InitNewPlayer(APlayerController* NewPlayerController, const FUniqueNetIdRepl& UniqueId, const FString& Options, const FString& Portal) override;
	virtual void HandleStartingNewPlayer_Implementation(APlayerController* NewPlayer) override;
	virtual void Logout(AController* Exiting) override;

	/** A server-side player: a bot, new or handed over by a neighbour (Restore). */
	AGasLabCharacter* SpawnBot(const FString& Id, const FString& Name, const FVector& At, float Yaw, const FGasLabWirePlayer* Restore);
	/** The state of a client on its way here has come: spawn it if it is waiting. */
	void ArrivalReady(const FString& Id);

private:
	AGasLabCharacter* SpawnFor(AController* Controller, const FString& Id, const FString& Name, const FVector& At, float Yaw, const FGasLabWirePlayer* Restore);
	void StartPlayer(APlayerController* PC, bool bGiveUp);
	void Scenario(float Seconds);
	void ThrowFrom(const FString& Id, const FVector& Target);
	void WalkBot(const FString& Id, const FVector& To);
	void LogStates();

	int32 Region = 0;
	FString ScenarioName;
	double StartedAt = 0;
	TSet<FString> Done;
	TMap<TWeakObjectPtr<APlayerController>, double> Waiting;
	int32 SoakBots = 0;
	double NextStateLog = 0, NextSoak = 0, NextAnnounce = 0;
	FRandomStream Random;
};
