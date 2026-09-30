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

	/** How much bigger than the 480-pixel layout the HUD is drawn on this screen. */
	float UiScale = 1.f;

public:
	virtual void DrawHUD() override;

	/** The Esc menu's buttons: 1 for Resume, 2 for Exit, 0 when the point (in screen pixels) is on neither or the menu is shut. */
	int32 MenuButtonAt(FVector2D ScreenPoint) const;

private:
	void DrawCentered(const FString& Text, float Y, float Scale, FLinearColor Color);
	void DrawIcon(FName Kind, float X, float Y, float Size, float Alpha);
	void DrawHearts(float X, float Y, double Health);
	/** Draws a menu button and remembers where it is on the screen, for a click to find it. */
	void DrawButton(const FString& Label, float Y, FBox2D& OutRect, FVector2D Mouse);

	FBox2D ResumeRect = FBox2D(ForceInit), ExitRect = FBox2D(ForceInit);
};
