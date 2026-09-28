#include "CubeWorldGameMode.h"
#include "CubePlayerPawn.h"
#include "CubeHUD.h"
#include "CubeWorldActor.h"

ACubeWorldGameMode::ACubeWorldGameMode()
{
	DefaultPawnClass = ACubePlayerPawn::StaticClass();
	HUDClass = ACubeHUD::StaticClass();
}

void ACubeWorldGameMode::BeginPlay()
{
	Super::BeginPlay();
	GetWorld()->SpawnActor<ACubeWorldActor>();
}
