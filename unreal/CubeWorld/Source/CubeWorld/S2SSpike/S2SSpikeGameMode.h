// The game mode of the spike's servers (?game=/Script/CubeWorld.S2SSpikeGameMode): none of Cube World's logic, no
// platform, no pawn. Players who join (the headless clients of a relay check) only watch.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "S2SSpikeGameMode.generated.h"

UCLASS()
class AS2SSpikeGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AS2SSpikeGameMode();
};
