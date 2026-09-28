// The heads-up display, drawn straight on the canvas: crosshair, the nine-slot hotbar with block icons
// and counts (dimmed while a bomb is in the hand), hearts, the server banner, the player list, the log, the hurt vignette, the start
// screen and the death screen.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "CubeHUD.generated.h"

class UCubeWorldGameInstance;
class ACubePlayerPawn;

UCLASS()
class CUBEWORLD_API ACubeHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;

private:
	void DrawCentered(const FString& Text, float Y, float Scale, FLinearColor Color);
	void DrawIcon(FName Kind, float X, float Y, float Size, float Alpha);
	void DrawHearts(float X, float Y, double Health);
};
