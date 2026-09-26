#include "CubeHUD.h"
#include "CanvasTypes.h"
#include "CubeWorld.h"
#include "CubeWorldGameInstance.h"
#include "CubePlayerPawn.h"
#include "CubeSpec.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/GameViewportClient.h"
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

// The panel the web client draws over its view (web/index.html, renderPanel): the live regions' servers, each with how
// many players it holds now, the line "Name — you are here" under the server the player is on, then the players with
// their health and the colour of the server they are on. The counts come from the presence list, which the server
// refreshes ten times a second, so a crossing shows as soon as the player has moved. Sized for the 480-pixel layout.
void ACubeHUD::DrawPanel()
{
	UCubeWorldGameInstance* Game = Cast<UCubeWorldGameInstance>(GetGameInstance());
	if (!Game) return;
	const float X = 8, Y = 8, Width = 148, Pad = 6, Head = 7, Name = 8.5f, Small = 7;
	const FLinearColor Grey(0.54f, 0.63f, 0.71f), Light(0.80f, 0.84f, 0.88f);
	TArray<FCubeRegion> Servers = Game->Regions;
	Servers.Sort([](const FCubeRegion& A, const FCubeRegion& B) { return A.Region < B.Region; });
	const float HeadH = TextSize(TEXT("SERVERS"), Head, true).Y, NameH = TextSize(TEXT("Ag"), Name, true).Y, SmallH = TextSize(TEXT("Ag"), Small).Y;
	const float RowH = NameH + 4, RowHereH = NameH + SmallH + 4, LineH = NameH + 2;

	float Height = Pad + HeadH + 3 + Pad + HeadH + 3 + Pad;
	// A row says "you are here" under the room the player is in, or why a room holds them out (an operator's doing).
	const auto Under = [Game](const FCubeRegion& S) -> FString
	{
		if (!Game->Room.IsEmpty() && S.Color == Game->Color) return FString::Printf(TEXT("%s — you are here"), *Game->PlayerName);
		const FCubeRefusal* Turned = Game->Barred.Find(S.Room);
		return Turned ? Turned->Title : FString();
	};
	for (const FCubeRegion& R : Servers) Height += (Under(R).IsEmpty() ? RowH : RowHereH) + 2;
	if (Servers.Num() == 0) Height += LineH;
	Height += FMath::Max(1, Game->Players.Num()) * LineH;
	DrawRect(FLinearColor(0.03f, 0.05f, 0.07f, 0.55f), X, Y, Width, Height);

	const float L = X + Pad, R = X + Width - Pad;
	float CY = Y + Pad;
	DrawLabel(TEXT("SERVERS"), L, CY, Head, Grey, true);
	CY += HeadH + 3;
	if (Servers.Num() == 0) { DrawLabel(TEXT("Not connected"), L, CY, Small, Grey); CY += LineH; }
	for (const FCubeRegion& S : Servers)
	{
		const FString Line = Under(S);
		const bool bBarred = Game->Barred.Contains(S.Room) && !(S.Color == Game->Color && !Game->Room.IsEmpty());
		const float H = Line.IsEmpty() ? RowH : RowHereH;
		int32 Count = 0;
		for (const FCubePresence& P : Game->Players) if (P.Color == S.Color) Count++;
		DrawRect(FLinearColor(0.09f, 0.14f, 0.20f, 0.6f), L, CY, R - L, H);
		DrawRect(ServerColor(S.Color), L, CY, 3, H);
		DrawLabel(S.Room, L + 7, CY + 2, Name, FLinearColor::White, true);
		const FString CountText = FString::FromInt(Count);
		DrawLabel(CountText, R - 4 - TextSize(CountText, Name, true).X, CY + 2, Name, FLinearColor::White, true);
		if (!Line.IsEmpty()) DrawLabel(Line, L + 7, CY + 2 + NameH, Small, bBarred ? FLinearColor(0.99f, 0.65f, 0.65f) : Light);
		CY += H + 2;
	}

	CY += Pad - 2;
	DrawLabel(TEXT("PLAYERS"), L, CY, Head, Grey, true);
	CY += HeadH + 3;
	if (Game->Players.Num() == 0) DrawLabel(TEXT("Not connected"), L, CY, Small, Grey);
	for (const FCubePresence& P : Game->Players)
	{
		const FString Health = P.Health <= 0 ? FString(TEXT("dead")) : FString::Printf(TEXT("%d hp"), FMath::CeilToInt32(P.Health));
		const float ColorW = TextSize(P.Color, Small).X, HealthX = R - ColorW - 6 - TextSize(Health, Small).X;
		DrawLabel(P.Color, R - ColorW, CY + 1, Small, ServerColor(P.Color));
		DrawLabel(Health, HealthX, CY + 1, Small, Grey);
		// A long name is cut short with an ellipsis rather than run into the health.
		// " (you)" always stays.
		const FString You = P.Id == Game->PlayerId ? FString(TEXT(" (you)")) : FString();
		FString Who = P.Name;
		if (TextSize(Who + You, Name).X > HealthX - 4 - L)
		{
			while (Who.Len() > 1 && TextSize(Who + TEXT("…") + You, Name).X > HealthX - 4 - L) Who.LeftChopInline(1);
			Who += TEXT("…");
		}
		DrawLabel(Who + You, L, CY, Name, FLinearColor::White);
		CY += LineH;
	}
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

	// The panel, top left: the servers and the players. The console key hides it.
	if (Game->IsInPlay() && !Game->bPanelHidden) DrawPanel();

	if (Pawn && FParse::Param(FCommandLine::Get(), TEXT("debughud")))
	{
		const FVector Cam = Pawn->FindComponentByClass<UCameraComponent>() ? Pawn->FindComponentByClass<UCameraComponent>()->GetComponentLocation() : FVector::ZeroVector;
		const FString Debug = FString::Printf(TEXT("body %.2f %.2f %.2f  actor %s  camera %s  view %s  ground %d"), Pawn->Body.X, Pawn->Body.Y, Pawn->Body.Z, *Pawn->GetActorLocation().ToString(), *Cam.ToString(), *Pawn->GetControlRotation().ToString(), Pawn->Body.bOnGround);
		DrawLabel(Debug, 12, 60, 8, FLinearColor::Yellow);
	}

	// While the player stands in the region of a room that holds them out, a line at the top says why (#barred on the web).
	const FString BarredLine = Game->IsInPlay() ? Game->BarredLine() : FString();
	if (!BarredLine.IsEmpty())
	{
		const FVector2D BS = TextSize(BarredLine, 11, true);
		DrawRect(FLinearColor(0.1f, 0.1f, 0.1f, 0.8f), W / 2 - BS.X / 2 - 9, 10, BS.X + 18, BS.Y + 10);
		DrawRect(FLinearColor(0.47f, 0.08f, 0.08f, 0.8f), W / 2 - BS.X / 2 - 8, 11, BS.X + 16, BS.Y + 8);
		DrawCentered(BarredLine, 15, 11, FLinearColor::White, true);
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
	else if (Game->Notice.IsSet())
	{
		// An operator's close or removal, over the game until OK, Enter or Esc (#notice on the web). The mouse is free meanwhile.
		FVector2D Mouse(-1, -1);
		if (APlayerController* PC = GetOwningPlayerController()) { float MX, MY; if (PC->GetMousePosition(MX, MY)) Mouse = FVector2D(MX, MY) / UiScale; }
		DrawRect(FLinearColor(0, 0, 0, 0.65f), 0, 0, W, H);
		DrawCentered(Game->Notice.Title, H / 2 - 60, 24, FLinearColor::White, true);
		DrawCentered(Game->Notice.Message, H / 2 - 22, 11, FLinearColor(0.81f, 0.85f, 0.89f), true);
		DrawButton(TEXT("OK"), H / 2 + 8, OkRect, Mouse);
	}
	else if (!Game->bPlaced)
	{
		// Until the player is placed: a welcome comes before the world does, and nothing of the game shows meanwhile.
		DrawRect(FLinearColor(0, 0, 0, 0.5f), 0, 0, W, H);
		DrawCentered(TEXT("Cube World"), H / 2 - 90, 36, FLinearColor::White, true);
		DrawCentered(FString::Printf(TEXT("Playing as %s  (start with -name=YourName to change it)"), *Game->PlayerName), H / 2 - 24, 11, FLinearColor(0.8f, 0.85f, 0.9f));
		DrawCentered(Game->Status, H / 2 + 4, 15, FLinearColor::White, true);
		DrawCentered(TEXT("WASD move, mouse look, Space jump, Shift sprint, Ctrl sneak. Hold left click to break, right click places, 1-9 or the wheel picks a block, Esc opens the menu."), H / 2 + 50, 9, FLinearColor(0.7f, 0.75f, 0.8f));
		DrawCentered(TEXT("Left click a player to hit them. Bombs come down on parachutes: walk into one to pick it up, right click throws it. The ` key hides the panel and shows it again."), H / 2 + 64, 9, FLinearColor(0.7f, 0.75f, 0.8f));
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
	}
	if (!Game->Notice.IsSet()) OkRect = FBox2D(ForceInit);
	if (Game->Notice.IsSet() || !Pawn || Pawn->bMouseCaptured || Game->bDead || !Game->bPlaced) ResumeRect = ExitRect = FBox2D(ForceInit);

	// The curtain, over everything: down from Play (or a jump from the server list) until the player stands where the
	// server put them, then it lifts and the game fades in. Its title and status stand where the start screen has them,
	// so going from one to the other moves nothing.
	const bool bDown = Game->bEntering || Now < Game->CurtainUntil;
	const float Dt = FMath::Min(GetWorld()->GetDeltaSeconds(), 0.05f);
	Game->Curtain = bDown ? FMath::Min(1.f, Game->Curtain + Dt / 0.3f) : FMath::Max(0.f, Game->Curtain - Dt / 0.5f);
	// Under the whole curtain the world is not drawn at all: the next map's first frames, before its HUD is made, show
	// the viewport's black, the curtain's own colour, instead of the world from the server's spawn point.
	if (UGameViewportClient* Viewport = GetWorld()->GetGameViewport()) Viewport->bDisableWorldRendering = Game->Curtain >= 1.f;
	if (Game->Curtain > 0.f)
	{
		const float A = FMath::SmoothStep(0.f, 1.f, Game->Curtain);
		DrawRect(FLinearColor(0, 0, 0, A), 0, 0, W, H);
		DrawCentered(TEXT("Cube World"), H / 2 - 90, 36, FLinearColor(1, 1, 1, A), true);
		// After the welcome the world still streams in, and that takes seconds from an Unreal server.
		const FString Line = !Game->Status.IsEmpty() ? Game->Status
			: Game->IsConnected() ? FString::Printf(TEXT("Loading the world... %d%%"), FMath::RoundToInt32(Game->WorldLoaded() * 100)) : FString();
		if (bDown) DrawCentered(Line, H / 2 + 4, 15, FLinearColor(1, 1, 1, A), true);
	}
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
	if (OkRect.bIsValid && OkRect.IsInsideOrOn(ScreenPoint)) return 3;
	return 0;
}
