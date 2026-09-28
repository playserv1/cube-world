#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "CubeWorldGameMode.generated.h"

UCLASS()
class CUBEWORLD_API ACubeWorldGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ACubeWorldGameMode();
	virtual void BeginPlay() override;
};
