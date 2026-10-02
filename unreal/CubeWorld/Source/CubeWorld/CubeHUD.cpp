#include "CubeHUD.h"
#include "CanvasTypes.h"
#include "CubeWorld.h"
#include "CubeWorldGameInstance.h"
#include "CubePlayerPawn.h"
#include "CubeSpec.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "EngineFontServices.h"
#include "Fonts/FontMeasure.h"
#include "Camera/CameraComponent.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace
{
	FLinearColor ServerColor(const FString& Name)
	{
		if (Name == TEXT("red")) return FLinearColor(0.94f, 0.27f, 0.27f);
		if (Name == TEXT("blue")) return FLinearColor(0.23f, 0.51f, 0.96f);
		if (Name == TEXT("green")) return FLinearColor(0.13f, 0.77f, 0.37f);
		if (Name == TEXT("yellow")) return FLinearColor(0.92f, 0.70f, 0.03f);
		if (Name == TEXT("purple")) return FLinearColor(0.66f, 0.33f, 0.97f);
		if (Name == TEXT("pink")) return FLinearColor(0.93f, 0.28f, 0.60f);
		return FLinearColor(0.6f, 0.64f, 0.69f);
	}
}

// The text is drawn outside the layout scale, at the real screen size: a font scaled up with the rest of the HUD is a
// small bitmap stretched 2-4 times and comes out blurred. Positions and sizes stay in layout pixels.
FSlateFontInfo ACubeHUD::UiFont(float Size, bool bBold) const
{
	return FSlateFontInfo(GEngine->GetLargeFont(), FMath::RoundToFloat(Size * UiScale), bBold ? FName(TEXT("Bold")) : FName(TEXT("Regular")));
}

FVector2D ACubeHUD::TextSize(const FString& Text, float Size, bool bBold) const
{
	return FEngineFontServices::Get().GetFontMeasure()->Measure(Text, UiFont(Size, bBold)) / UiScale;
}

void ACubeHUD::DrawLabel(const FString& Text, float X, float Y, float Size, FLinearColor Color, bool bBold)
{
	Canvas->Canvas->PushAbsoluteTransform(FMatrix::Identity);
	FCanvasTextItem Item(FVector2D(FMath::RoundToFloat(X * UiScale), FMath::RoundToFloat(Y * UiScale)), FText::FromString(Text), UiFont(Size, bBold), Color);
	Item.EnableShadow(FLinearColor::Black, FVector2D(FMath::Max(1.f, FMath::RoundToFloat(UiScale * 0.5f))));
	Canvas->Canvas->DrawItem(Item);
	Canvas->Canvas->PopTransform();
}

void ACubeHUD::DrawCentered(const FString& Text, float Y, float Size, FLinearColor Color, bool bBold)
{
	DrawLabel(Text, Canvas->SizeX / UiScale / 2 - TextSize(Text, Size, bBold).X / 2, Y, Size, Color, bBold);
}

// The block as a flat icon: its top face above its side face, lit as in the world.
void ACubeHUD::DrawIcon(FName Kind, float X, float Y, float Size, float Alpha)
{
	UCubeWorldGameInstance* Game = Cast<UCubeWorldGameInstance>(GetGameInstance());
	if (!Game || !Game->Textures.Atlas) return;
	const FCubeFaces Faces = Game->Textures.Faces(Kind, TEXT("green"));
	const FCubeTileUV Top = Game->Textures.UV(Faces.Top), Side = Game->Textures.UV(Faces.Side);
	DrawTexture(Game->Textures.Atlas, X, Y, Size, Size * 0.4f, Top.U0, Top.V0, Top.U1 - Top.U0, Top.V1 - Top.V0, FLinearColor(1, 1, 1, Alpha));
	DrawTexture(Game->Textures.Atlas, X, Y + Size * 0.4f, Size, Size * 0.6f, Side.U0, Side.V0, Side.U1 - Side.U0, Side.V1 - Side.V0, FLinearColor(0.8f, 0.8f, 0.8f, Alpha));
}

void ACubeHUD::DrawHearts(float X, float Y, double Health)
{
	const int32 HP = FMath::CeilToInt32(Health);
	for (int32 I = 0; I < 10; I++)
	{
		const float HX = X + I * 20.f;
		DrawRect(FLinearColor(0.23f, 0.04f, 0.04f), HX, Y, 18, 16);
		if (HP >= 2 * I + 2) DrawRect(FLinearColor(0.88f, 0.13f, 0.11f), HX + 1, Y + 1, 16, 14);
		else if (HP >= 2 * I + 1) DrawRect(FLinearColor(0.88f, 0.13f, 0.11f), HX + 1, Y + 1, 8, 14);
	}
}

void ACubeHUD::DrawHUD()
{
	if (CubeIsServerProcess()) return;   // a headless server draws nothing
	Super::DrawHUD();
	UCubeWorldGameInstance* Game = Cast<UCubeWorldGameInstance>(GetGameInstance());
	ACubePlayerPawn* Pawn = Cast<ACubePlayerPawn>(GetOwningPawn());
	if (!Game || !Canvas) return;
	// The HUD is laid out for a 480-pixel-high screen and scaled up to the real one: at 1080p everything is 2.25 times
	// as big, so it reads at any resolution instead of shrinking as the window grows.
	UiScale = FMath::Clamp(Canvas->SizeY / 480.f, 1.f, 4.f);
	Canvas->Canvas->PushAbsoluteTransform(FScaleMatrix(FVector(UiScale, UiScale, 1.f)));
	struct FPopScale { FCanvas* C; ~FPopScale() { C->PopTransform(); } } PopScale{ Canvas->Canvas };
	const float W = Canvas->SizeX / UiScale, H = Canvas->SizeY / UiScale;
	const double Now = FPlatformTime::Seconds();

	// Hurt vignette.
	if (Pawn && Now < Pawn->HurtUntil)
	{
		const FLinearColor Red(0.85f, 0, 0, 0.35f);
		DrawRect(Red, 0, 0, W, 40); DrawRect(Red, 0, H - 40, W, 40); DrawRect(Red, 0, 0, 40, H); DrawRect(Red, W - 40, 0, 40, H);
	}

	// Crosshair.
	if (Game->IsInPlay() && !Game->bDead)
	{
		DrawRect(FLinearColor(1, 1, 1, 0.9f), W / 2 - 1, H / 2 - 9, 2, 18);
		DrawRect(FLinearColor(1, 1, 1, 0.9f), W / 2 - 9, H / 2 - 1, 18, 2);
	}

	// Server banner and the players, top left and right.
	if (Game->IsInPlay())
	{
		const FString Banner = FString::Printf(TEXT("you are on server %s-%s"), *Game->Color, *Game->Server);
		const FVector2D BS = TextSize(Banner, 10); const float BW = BS.X, BH = BS.Y;
		DrawRect(FLinearColor(0, 0, 0, 0.45f), 12, 12, BW + 24, BH + 10);
		DrawRect(ServerColor(Game->Color), 12, 12, 6, BH + 10);
		DrawLabel(Banner, 26, 17, 10, FLinearColor::White);

		float PY = 12;
		for (const FCubePresence& P : Game->Players)
		{
			const FString Health = P.Health <= 0 ? FString(TEXT("dead")) : FString::Printf(TEXT("%d hp"), FMath::CeilToInt32(P.Health));
			const FString Line = FString::Printf(TEXT("%s%s  %s  %s"), *P.Name, P.Id == Game->PlayerId ? TEXT(" (you)") : TEXT(""), *Health, *P.Color);
			const FVector2D LS = TextSize(Line, 10); const float LW = LS.X, LH = LS.Y;
			DrawLabel(Line, W - LW - 12, PY, 10, ServerColor(P.Color));
			PY += LH + 2;
		}
	}

	if (Pawn && FParse::Param(FCommandLine::Get(), TEXT("debughud")))
	{
		const FVector Cam = Pawn->FindComponentByClass<UCameraComponent>() ? Pawn->FindComponentByClass<UCameraComponent>()->GetComponentLocation() : FVector::ZeroVector;
		const FString Debug = FString::Printf(TEXT("body %.2f %.2f %.2f  actor %s  camera %s  view %s  ground %d"), Pawn->Body.X, Pawn->Body.Y, Pawn->Body.Z, *Pawn->GetActorLocation().ToString(), *Cam.ToString(), *Pawn->GetControlRotation().ToString(), Pawn->Body.bOnGround);
		DrawLabel(Debug, 12, 60, 8, FLinearColor::Yellow);
	}

	// Log, bottom left.
	float LY = H - 24;
	for (const FString& Line : Game->LogLines)
	{
		DrawLabel(Line, 12, LY, 9, FLinearColor(0.7f, 0.75f, 0.8f, 0.9f));
		LY -= 14;
	}

	// Hearts and hotbar, bottom centre.
	const float SlotSize = 44, Gap = 2, BarW = 9 * SlotSize + 8 * Gap + 8;
	const float BarX = W / 2 - BarW / 2, BarY = H - SlotSize - 14;
	if (Game->IsInPlay())
	{
		// With a bomb in the hand the hotbar steps back: right click throws the bomb, it places nothing.
		const bool bHolding = !Game->Holding.IsEmpty();
		const float Dim = bHolding ? 0.55f : 1.f;
		DrawHearts(BarX + 4, BarY - 24, Game->Health);
		DrawRect(FLinearColor(0, 0, 0, 0.55f * Dim), BarX, BarY, BarW, SlotSize + 8);
		for (int32 I = 0; I < 9; I++)
		{
			const float SX = BarX + 4 + I * (SlotSize + Gap), SY = BarY + 4;
			DrawRect(I == Game->Slot ? FLinearColor(1, 1, 1, Dim) : FLinearColor(0.33f, 0.33f, 0.33f, Dim), SX, SY, SlotSize, SlotSize);
			DrawRect(FLinearColor(0.15f, 0.15f, 0.15f, 0.9f * Dim), SX + 2, SY + 2, SlotSize - 4, SlotSize - 4);
			if (!Game->Hotbar.IsValidIndex(I)) continue;
			const FName Kind = Game->Hotbar[I];
			const int32 Count = Game->Inventory.FindRef(Kind);
			DrawIcon(Kind, SX + 6, SY + 6, SlotSize - 12, (Count > 0 ? 1.f : 0.3f) * Dim);
			const FString CountText = FString::FromInt(Count);
			const FVector2D CS = TextSize(CountText, 9, true); const float CW = CS.X, CH = CS.Y;
			DrawLabel(CountText, SX + SlotSize - CW - 4, SY + SlotSize - CH - 2, 9, FLinearColor::White, true);
		}
		if (bHolding) DrawCentered(TEXT("bomb - right click throws it"), BarY - 44, 11, FLinearColor::White, true);
		else if (Game->Hotbar.IsValidIndex(Game->Slot)) DrawCentered(Game->Hotbar[Game->Slot].ToString(), BarY - 44, 11, FLinearColor::White, true);
	}

	// The start screen and the death screen.
	if (Game->bDead)
	{
		DrawRect(FLinearColor(0.47f, 0, 0, 0.55f), 0, 0, W, H);
		DrawCentered(TEXT("You died!"), H / 2 - 44, 28, FLinearColor::White, true);
		DrawCentered(TEXT("Press Enter or click to respawn"), H / 2 + 10, 12, FLinearColor::White);
	}
	else if (!Game->IsInPlay() && !Game->bPlaced)
	{
		DrawRect(FLinearColor(0, 0, 0, 0.5f), 0, 0, W, H);
		DrawCentered(TEXT("Cube World"), H / 2 - 90, 36, FLinearColor::White, true);
		DrawCentered(FString::Printf(TEXT("Playing as %s  (start with -name=YourName to change it)"), *Game->PlayerName), H / 2 - 24, 11, FLinearColor(0.8f, 0.85f, 0.9f));
		DrawCentered(Game->Status, H / 2 + 4, 15, FLinearColor::White, true);
		DrawCentered(TEXT("WASD move, mouse look, Space jump, Shift sprint, Ctrl sneak. Hold left click to break, right click places, 1-9 or the wheel picks a block, Esc opens the menu."), H / 2 + 50, 9, FLinearColor(0.7f, 0.75f, 0.8f));
		DrawCentered(TEXT("Left click a player to hit them. Bombs come down on parachutes: walk into one to pick it up, right click throws it."), H / 2 + 64, 9, FLinearColor(0.7f, 0.75f, 0.8f));
	}
	else if (Pawn && !Pawn->bMouseCaptured)
	{
		// The Esc menu: the game goes on behind it, as in Minecraft on a server.
		FVector2D Mouse(-1, -1);
		if (APlayerController* PC = GetOwningPlayerController()) { float MX, MY; if (PC->GetMousePosition(MX, MY)) Mouse = FVector2D(MX, MY) / UiScale; }
		DrawRect(FLinearColor(0, 0, 0, 0.5f), 0, 0, W, H);
		DrawCentered(TEXT("Game menu"), H / 2 - 80, 20, FLinearColor::White, true);
		DrawButton(TEXT("Resume"), H / 2 - 30, ResumeRect, Mouse);
		DrawButton(TEXT("Exit"), H / 2 + 10, ExitRect, Mouse);
		return;
	}
	ResumeRect = ExitRect = FBox2D(ForceInit);
}

void ACubeHUD::DrawButton(const FString& Label, float Y, FBox2D& OutRect, FVector2D Mouse)
{
	const float BW = 200, BH = 30, X = Canvas->SizeX / UiScale / 2 - BW / 2;
	const bool bHover = Mouse.X >= X && Mouse.X <= X + BW && Mouse.Y >= Y && Mouse.Y <= Y + BH;
	DrawRect(FLinearColor(0.1f, 0.1f, 0.1f, 0.9f), X - 2, Y - 2, BW + 4, BH + 4);
	DrawRect(bHover ? FLinearColor(0.45f, 0.5f, 0.75f, 0.95f) : FLinearColor(0.35f, 0.35f, 0.38f, 0.95f), X, Y, BW, BH);
	const float TH = TextSize(Label, 12, true).Y;
	DrawCentered(Label, Y + BH / 2 - TH / 2, 12, bHover ? FLinearColor(1.f, 1.f, 0.63f) : FLinearColor::White, true);
	OutRect = FBox2D(FVector2D(X, Y) * UiScale, FVector2D(X + BW, Y + BH) * UiScale);
}

int32 ACubeHUD::MenuButtonAt(FVector2D ScreenPoint) const
{
	if (ResumeRect.bIsValid && ResumeRect.IsInsideOrOn(ScreenPoint)) return 1;
	if (ExitRect.bIsValid && ExitRect.IsInsideOrOn(ScreenPoint)) return 2;
	return 0;
}
