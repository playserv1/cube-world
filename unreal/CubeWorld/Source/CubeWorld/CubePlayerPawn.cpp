#include "CubePlayerPawn.h"
#include "CubeWorld.h"
#include "CubeWorldGameInstance.h"
#include "CubeAvatar.h"
#include "CubeWorldActor.h"
#include "Camera/CameraComponent.h"
#include "Components/InputComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "Kismet/GameplayStatics.h"
#include "DrawDebugHelpers.h"
#include "Dom/JsonObject.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "EngineUtils.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "TimerManager.h"
#include "UnrealClient.h"

ACubePlayerPawn::ACubePlayerPawn()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(RootComponent);
	Camera->bUsePawnControlRotation = true;
	Camera->SetFieldOfView(CubeSpec::Fov);
	bUseControllerRotationYaw = false;
	bUseControllerRotationPitch = false;
}

void ACubePlayerPawn::BeginPlay()
{
	Super::BeginPlay();
	Game = Cast<UCubeWorldGameInstance>(GetGameInstance());
	if (!Game) return;
	SkinMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Materials/M_Skin.M_Skin"));
	if (!SkinMaterial) SkinMaterial = UMaterial::GetDefaultMaterial(MD_Surface);

	Game->OnWelcome.AddUObject(this, &ACubePlayerPawn::HandleWelcome);
	Game->OnRespawn.AddUObject(this, &ACubePlayerPawn::HandleRespawn);
	Game->OnHurt.AddUObject(this, &ACubePlayerPawn::HandleHurt);
	Game->OnDeath.AddUObject(this, &ACubePlayerPawn::HandleDeath);
	Game->OnPlayers.AddUObject(this, &ACubePlayerPawn::HandlePlayers);
	Game->OnCube.AddUObject(this, &ACubePlayerPawn::HandleCube);

	Body.Teleport(36, 12, 0);
	if (APlayerController* PC = Cast<APlayerController>(GetController()))
	{
		PC->SetControlRotation(FRotator(-15.f, 135.f, 0));
		PC->PlayerCameraManager->ViewPitchMin = -89.9f;
		PC->PlayerCameraManager->ViewPitchMax = 89.9f;
	}
	CaptureMouse(false);

	FString Name;
	if (!FParse::Value(FCommandLine::Get(), TEXT("-name="), Name) || Name.IsEmpty())
		Name = FString::Printf(TEXT("Player-%04d"), FMath::RandRange(0, 9999));
	Game->PlayerName = Name;

	// Unattended runs: -autoplay signs in at once, -screenshot=<s> saves Saved/Screenshots/cube.png after
	// that many seconds, -quitafter=<s> ends the process.
	if (FParse::Param(FCommandLine::Get(), TEXT("autoplay")))
	{
		FTimerHandle Handle;
		GetWorldTimerManager().SetTimer(Handle, [this]() { if (Game) Game->StartPlay(Game->PlayerName); }, 1.f, false);
	}
	float Seconds = 0;
	if (FParse::Value(FCommandLine::Get(), TEXT("-screenshot="), Seconds) && Seconds > 0)
	{
		FTimerHandle Handle;
		GetWorldTimerManager().SetTimer(Handle, []() { FScreenshotRequest::RequestScreenshot(TEXT("cube.png"), false, false); }, Seconds, false);
	}
	// -selftest: after the welcome, walk forward for a second, place a block ahead, dig it back, hit whoever is near.
	if (FParse::Param(FCommandLine::Get(), TEXT("selftest")))
	{
		FTimerHandle H1, H2, H3, H4, H5;
		GetWorldTimerManager().SetTimer(H1, [this]() { bMouseCaptured = true; TestForward = 1.f; Game->Log(FString::Printf(TEXT("selftest: walking from %.2f %.2f"), Body.X, Body.Y)); }, 5.f, false);
		GetWorldTimerManager().SetTimer(H2, [this]() { TestForward = 0.f; Game->Log(FString::Printf(TEXT("selftest: stopped at %.2f %.2f (ground %d)"), Body.X, Body.Y, Body.bOnGround)); }, 6.f, false);
		GetWorldTimerManager().SetTimer(H3, [this]()
		{
			const int32 X = FMath::FloorToInt32(Body.X) + 2, Y = FMath::FloorToInt32(Body.Y);
			const TSharedRef<FJsonObject> F = MakeShared<FJsonObject>();
			F->SetStringField(TEXT("op"), TEXT("place")); F->SetNumberField(TEXT("x"), X); F->SetNumberField(TEXT("y"), Y); F->SetNumberField(TEXT("z"), -1);
			F->SetNumberField(TEXT("nx"), 0); F->SetNumberField(TEXT("ny"), 0); F->SetNumberField(TEXT("nz"), 1); F->SetStringField(TEXT("kind"), TEXT("gold"));
			Game->Send(F);
			Game->Log(FString::Printf(TEXT("selftest: placed gold at %d %d 0"), X, Y));
		}, 7.f, false);
		GetWorldTimerManager().SetTimer(H4, [this]()
		{
			const int32 X = FMath::FloorToInt32(Body.X) + 2, Y = FMath::FloorToInt32(Body.Y);
			Game->Log(FString::Printf(TEXT("selftest: world says %s at %d %d 0, gold left %d"), *Game->World.KindAt(X, Y, 0).ToString(), X, Y, Game->Inventory.FindRef(TEXT("gold"))));
			const TSharedRef<FJsonObject> F = MakeShared<FJsonObject>();
			F->SetStringField(TEXT("op"), TEXT("dig")); F->SetStringField(TEXT("state"), TEXT("start")); F->SetNumberField(TEXT("x"), X - 2); F->SetNumberField(TEXT("y"), Y); F->SetNumberField(TEXT("z"), -1);
			Game->Send(F);
			Game->Log(FString::Printf(TEXT("selftest: digging grass under the feet at %d %d -1"), X - 2, Y));
		}, 8.f, false);
		GetWorldTimerManager().SetTimer(H5, [this]()
		{
			const int32 X = FMath::FloorToInt32(Body.X) + 2, Y = FMath::FloorToInt32(Body.Y);
			Game->Log(FString::Printf(TEXT("selftest: after dig %s at %d %d -1, dirt %d, players seen %d"), *Game->World.KindAt(X - 2, Y, -1).ToString(), X - 2, Y, Game->Inventory.FindRef(TEXT("dirt")), Game->Players.Num()));
			for (const FCubePresence& P : Game->Players) if (P.Id != Game->PlayerId) { const TSharedRef<FJsonObject> F = MakeShared<FJsonObject>(); F->SetStringField(TEXT("op"), TEXT("attack")); F->SetStringField(TEXT("target"), P.Id); Game->Send(F); Game->Log(FString::Printf(TEXT("selftest: hit %s"), *P.Name)); }
		}, 11.f, false);
		Game->OnCube.AddLambda([this](int32 X, int32 Y, int32 Z, FName Kind) { Game->Log(FString::Printf(TEXT("cube %d %d %d -> %s"), X, Y, Z, *Kind.ToString())); });
		Game->OnDig.AddLambda([this](const FString&, int32 X, int32 Y, int32 Z, int32 Stage) { if (Stage <= 0 || Stage == 9) Game->Log(FString::Printf(TEXT("dig %d %d %d stage %d"), X, Y, Z, Stage)); });
	}
	// -frametest: send twenty placements the server must refuse, count the answers, and report.
	if (FParse::Param(FCommandLine::Get(), TEXT("frametest")))
	{
		static int32 Answers = 0;
		Game->OnInventory.AddLambda([]() { Answers++; });
		for (int32 I = 0; I < 20; I++)
		{
			FTimerHandle H;
			GetWorldTimerManager().SetTimer(H, [this]()
			{
				const TSharedRef<FJsonObject> F = MakeShared<FJsonObject>();
				F->SetStringField(TEXT("op"), TEXT("place")); F->SetNumberField(TEXT("x"), 36); F->SetNumberField(TEXT("y"), 13); F->SetNumberField(TEXT("z"), -1);
				F->SetNumberField(TEXT("nx"), 0); F->SetNumberField(TEXT("ny"), 0); F->SetNumberField(TEXT("nz"), 1); F->SetStringField(TEXT("kind"), TEXT("gold"));
				Game->Send(F);
			}, 5.f + I * 0.1f, false);
		}
		FTimerHandle Done;
		GetWorldTimerManager().SetTimer(Done, [this]() { Game->Log(FString::Printf(TEXT("frametest: %d of 20 refusals answered"), Answers)); }, 9.f, false);
	}
	if (FParse::Value(FCommandLine::Get(), TEXT("-quitafter="), Seconds) && Seconds > 0)
	{
		FTimerHandle Handle;
		GetWorldTimerManager().SetTimer(Handle, []() { FPlatformMisc::RequestExit(false); }, Seconds, false);
	}
}

ACubeWorldActor* ACubePlayerPawn::WorldActor() const
{
	for (TActorIterator<ACubeWorldActor> It(GetWorld()); It; ++It) return *It;
	return nullptr;
}

void ACubePlayerPawn::SetupPlayerInputComponent(UInputComponent* Input)
{
	Super::SetupPlayerInputComponent(Input);
	Input->BindAxis(TEXT("MoveForward"), this, &ACubePlayerPawn::OnMoveForward);
	Input->BindAxis(TEXT("MoveRight"), this, &ACubePlayerPawn::OnMoveRight);
	Input->BindAxis(TEXT("Turn"), this, &ACubePlayerPawn::OnTurn);
	Input->BindAxis(TEXT("LookUp"), this, &ACubePlayerPawn::OnLookUp);
	Input->BindAction<TDelegate<void(bool)>>(TEXT("Jump"), IE_Pressed, this, &ACubePlayerPawn::OnJump, true);
	Input->BindAction<TDelegate<void(bool)>>(TEXT("Jump"), IE_Released, this, &ACubePlayerPawn::OnJump, false);
	Input->BindAction<TDelegate<void(bool)>>(TEXT("Sprint"), IE_Pressed, this, &ACubePlayerPawn::OnSprint, true);
	Input->BindAction<TDelegate<void(bool)>>(TEXT("Sprint"), IE_Released, this, &ACubePlayerPawn::OnSprint, false);
	Input->BindAction<TDelegate<void(bool)>>(TEXT("Sneak"), IE_Pressed, this, &ACubePlayerPawn::OnSneak, true);
	Input->BindAction<TDelegate<void(bool)>>(TEXT("Sneak"), IE_Released, this, &ACubePlayerPawn::OnSneak, false);
	Input->BindAction<TDelegate<void(bool)>>(TEXT("Dig"), IE_Pressed, this, &ACubePlayerPawn::OnDig, true);
	Input->BindAction<TDelegate<void(bool)>>(TEXT("Dig"), IE_Released, this, &ACubePlayerPawn::OnDig, false);
	Input->BindAction(TEXT("Place"), IE_Pressed, this, &ACubePlayerPawn::OnPlace);
	for (int32 I = 1; I <= 9; I++)
		Input->BindAction<TDelegate<void(int32)>>(*FString::Printf(TEXT("Slot%d"), I), IE_Pressed, this, &ACubePlayerPawn::OnSlot, I - 1);
	Input->BindAction(TEXT("SlotNext"), IE_Pressed, this, &ACubePlayerPawn::OnSlotNext);
	Input->BindAction(TEXT("SlotPrevious"), IE_Pressed, this, &ACubePlayerPawn::OnSlotPrevious);
	Input->BindAction(TEXT("Confirm"), IE_Pressed, this, &ACubePlayerPawn::OnConfirm);
	Input->BindAction(TEXT("Release"), IE_Pressed, this, &ACubePlayerPawn::OnRelease);
}

void ACubePlayerPawn::OnTurn(float V) { if (bMouseCaptured) AddControllerYawInput(V * 0.12f); }
void ACubePlayerPawn::OnLookUp(float V) { if (bMouseCaptured) AddControllerPitchInput(V * 0.12f); }

void ACubePlayerPawn::CaptureMouse(bool bCapture)
{
	bMouseCaptured = bCapture;
	APlayerController* PC = Cast<APlayerController>(GetController());
	if (!PC) return;
	if (bCapture)
	{
		PC->SetInputMode(FInputModeGameOnly());
		PC->bShowMouseCursor = false;
	}
	else
	{
		FInputModeGameAndUI Mode;
		Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		Mode.SetHideCursorDuringCapture(false);
		PC->SetInputMode(Mode);
		PC->bShowMouseCursor = true;
	}
}

int32 ACubePlayerPawn::Slot() const { return Game ? Game->Slot : 0; }

void ACubePlayerPawn::OnSlot(int32 Index) { if (Game) Game->Slot = FMath::Clamp(Index, 0, 8); }

void ACubePlayerPawn::OnConfirm()
{
	if (!Game) return;
	if (Game->bDead) { const TSharedRef<FJsonObject> F = MakeShared<FJsonObject>(); F->SetStringField(TEXT("op"), TEXT("respawn")); Game->Send(F); return; }
	if (!Game->IsConnected() && !Game->IsSigningIn()) { Game->StartPlay(Game->PlayerName); return; }
	if (Game->IsConnected()) CaptureMouse(true);
}

void ACubePlayerPawn::OnRelease()
{
	CaptureMouse(false);
}

void ACubePlayerPawn::OnDig(bool bHeld)
{
	bDigHeld = bHeld;
	if (!Game || !bHeld) return;
	if (!bMouseCaptured)
	{
		if (Game->bDead) { OnConfirm(); return; }
		if (Game->IsConnected()) CaptureMouse(true);
		else OnConfirm();
		return;
	}
	// A click on a player is a hit; digging is handled on the tick while the button stays down.
	UpdateAim();
	if (Aim.bPlayer)
	{
		const TSharedRef<FJsonObject> F = MakeShared<FJsonObject>();
		F->SetStringField(TEXT("op"), TEXT("attack"));
		F->SetStringField(TEXT("target"), Aim.PlayerId);
		Game->Send(F);
	}
}

void ACubePlayerPawn::OnPlace()
{
	if (!Game || !bMouseCaptured || Game->bDead) return;
	UpdateAim();
	if (!Aim.bBlock) return;
	const FName Kind = Game->Hotbar.IsValidIndex(Slot()) ? Game->Hotbar[Slot()] : NAME_None;
	if (Kind == NAME_None || Game->Inventory.FindRef(Kind) <= 0) return;
	const TSharedRef<FJsonObject> F = MakeShared<FJsonObject>();
	F->SetStringField(TEXT("op"), TEXT("place"));
	F->SetNumberField(TEXT("x"), Aim.Hit.Block.X); F->SetNumberField(TEXT("y"), Aim.Hit.Block.Y); F->SetNumberField(TEXT("z"), Aim.Hit.Block.Z);
	F->SetNumberField(TEXT("nx"), Aim.Hit.Normal.X); F->SetNumberField(TEXT("ny"), Aim.Hit.Normal.Y); F->SetNumberField(TEXT("nz"), Aim.Hit.Normal.Z);
	F->SetStringField(TEXT("kind"), Kind.ToString());
	Game->Send(F);
}

void ACubePlayerPawn::Spawn(double X, double Y, double Z)
{
	Body.Teleport(X, Y, Z);
	Unstick();
	Game->bPlaced = true;
}

void ACubePlayerPawn::Unstick()
{
	const FCubeSolidQuery Solid = [this](int32 X, int32 Y, int32 Z) { return Game->World.IsSolid(X, Y, Z); };
	int32 Guard = 0;
	while (CubePhysics::Overlaps(Body, Solid) && Guard++ < 80) { Body.Z = FMath::Floor(Body.Z) + 1; Body.PZ = Body.Z; }
}

void ACubePlayerPawn::HandleWelcome(const FCubePose& You, bool bTeleport)
{
	if (bTeleport) Spawn(You.X, You.Y, You.Z);
	LastPose.Empty();
	for (auto& Pair : Avatars) if (Pair.Value) Pair.Value->Destroy();
	Avatars.Empty();
	if (!bMouseCaptured) CaptureMouse(true);
}

void ACubePlayerPawn::HandleRespawn(const FCubePose& You)
{
	Spawn(You.X, You.Y, You.Z);
	CaptureMouse(true);
}

void ACubePlayerPawn::HandleHurt(const FString& PlayerId, double, double KX, double KY, double Strength)
{
	if (PlayerId == Game->PlayerId)
	{
		HurtUntil = FPlatformTime::Seconds() + CubeSpec::HurtTicks * CubeSpec::TickSeconds;
		if (Strength > 0) CubePhysics::Knockback(Body, KX, KY, Strength);
		return;
	}
	if (ACubeAvatar** A = Avatars.Find(PlayerId)) if (*A) (*A)->Hurt();
}

void ACubePlayerPawn::HandleDeath(const FString& PlayerId, const FString& By)
{
	if (PlayerId == Game->PlayerId) { CaptureMouse(false); bDigging = false; }
	FString Who = PlayerId == Game->PlayerId ? TEXT("you") : PlayerId, Killer = By;
	for (const FCubePresence& P : Game->Players) { if (P.Id == PlayerId) Who = P.Name; if (P.Id == By) Killer = P.Name; }
	Game->Log(FString::Printf(TEXT("%s died%s"), *Who, By.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" to %s"), *Killer)));
}

void ACubePlayerPawn::HandleCube(int32 X, int32 Y, int32 Z, FName)
{
	if (bDigging && DigTarget == FIntVector(X, Y, Z) && Game->World.KindAt(X, Y, Z) == TEXT("air"))
	{
		bDigging = false;
		DigCooldown = CubeSpec::DigCooldownTicks;
	}
}

void ACubePlayerPawn::HandlePlayers(const TArray<FCubePresence>& Players)
{
	TSet<FString> Seen;
	for (const FCubePresence& P : Players)
	{
		if (P.Id == Game->PlayerId) continue;
		Seen.Add(P.Id);
		ACubeAvatar** Found = Avatars.Find(P.Id);
		ACubeAvatar* Avatar = Found ? *Found : nullptr;
		if (!Avatar)
		{
			Avatar = GetWorld()->SpawnActor<ACubeAvatar>();
			Avatar->Setup(P.Id, P.Name, SkinMaterial);
			Avatars.Add(P.Id, Avatar);
			Game->Log(FString::Printf(TEXT("%s appeared at %.1f %.1f %.1f"), *P.Name, P.X, P.Y, P.Z));
		}
		Avatar->SetTarget(P.X, P.Y, P.Z, P.Yaw, P.Pitch, P.bSneaking, P.Health);
	}
	for (auto It = Avatars.CreateIterator(); It; ++It)
		if (!Seen.Contains(It.Key())) { if (It.Value()) It.Value()->Destroy(); It.RemoveCurrent(); }
}

void ACubePlayerPawn::UpdateAim()
{
	Aim = FCubeAim();
	if (!Game || !Game->bPlaced) return;
	const FVector Eye(Body.X, Body.Y, Body.Z + Body.EyeHeight());
	const FVector Direction = Camera->GetForwardVector();
	FCubeRayHit Hit;
	const bool bBlock = Game->World.Raycast(Eye, Direction, CubeSpec::BlockReach, Hit);

	double Best = CubeSpec::EntityReach;
	FString BestId;
	for (const auto& Pair : Avatars)
	{
		const ACubeAvatar* A = Pair.Value;
		if (!A) continue;
		const FVector P = A->GetActorLocation() / CubeSpec::BlockCm;
		const double H = A->bSneaking ? CubeSpec::SneakHeight : CubeSpec::Height;
		const FBox Box(FVector(P.X - 0.3, P.Y - 0.3, P.Z), FVector(P.X + 0.3, P.Y + 0.3, P.Z + H));
		FVector HitLocation, HitNormal; float Time;
		if (FMath::LineExtentBoxIntersection(Box, Eye, Eye + Direction * CubeSpec::EntityReach, FVector::ZeroVector, HitLocation, HitNormal, Time))
		{
			const double D = Time * CubeSpec::EntityReach;
			if (D < Best) { Best = D; BestId = Pair.Key; }
		}
	}
	if (!BestId.IsEmpty() && (!bBlock || Best < Hit.Distance)) { Aim.bPlayer = true; Aim.PlayerId = BestId; return; }
	if (bBlock) { Aim.bBlock = true; Aim.Hit = Hit; }
}

void ACubePlayerPawn::DigTick()
{
	if (DigCooldown > 0) { DigCooldown--; return; }
	const bool bWant = bDigHeld && bMouseCaptured && !Game->bDead && Aim.bBlock;
	const FIntVector Target = bWant ? Aim.Hit.Block : FIntVector::ZeroValue;
	if (bWant == bDigging && (!bWant || Target == DigTarget)) return;
	if (bDigging)
	{
		const TSharedRef<FJsonObject> Stop = MakeShared<FJsonObject>();
		Stop->SetStringField(TEXT("op"), TEXT("dig")); Stop->SetStringField(TEXT("state"), TEXT("stop"));
		Stop->SetNumberField(TEXT("x"), DigTarget.X); Stop->SetNumberField(TEXT("y"), DigTarget.Y); Stop->SetNumberField(TEXT("z"), DigTarget.Z);
		Game->Send(Stop);
	}
	bDigging = bWant;
	DigTarget = Target;
	if (bWant)
	{
		const TSharedRef<FJsonObject> Start = MakeShared<FJsonObject>();
		Start->SetStringField(TEXT("op"), TEXT("dig")); Start->SetStringField(TEXT("state"), TEXT("start"));
		Start->SetNumberField(TEXT("x"), Target.X); Start->SetNumberField(TEXT("y"), Target.Y); Start->SetNumberField(TEXT("z"), Target.Z);
		Game->Send(Start);
	}
}

void ACubePlayerPawn::SendMove(double Yaw, double Pitch)
{
	const FString Pose = FString::Printf(TEXT("%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d,%d"), Body.X, Body.Y, Body.Z, Yaw, Pitch, Body.bOnGround, Body.bSneaking, Body.bSprinting);
	if (Pose == LastPose) return;
	LastPose = Pose;
	const TSharedRef<FJsonObject> F = MakeShared<FJsonObject>();
	F->SetStringField(TEXT("op"), TEXT("move"));
	F->SetNumberField(TEXT("x"), Body.X); F->SetNumberField(TEXT("y"), Body.Y); F->SetNumberField(TEXT("z"), Body.Z);
	F->SetNumberField(TEXT("yaw"), Yaw); F->SetNumberField(TEXT("pitch"), Pitch);
	F->SetBoolField(TEXT("onGround"), Body.bOnGround); F->SetBoolField(TEXT("sneaking"), Body.bSneaking); F->SetBoolField(TEXT("sprinting"), Body.bSprinting);
	Game->Send(F);
}

void ACubePlayerPawn::GameTick()
{
	if (!Game->bPlaced) return;
	if (!Game->bDead)
	{
		// Minecraft's yaw is Unreal's yaw minus 90°; its pitch is positive looking down.
		const FRotator View = GetControlRotation();
		const double Yaw = FMath::DegreesToRadians(FRotator::NormalizeAxis(View.Yaw - 90.f));
		const double Pitch = -FMath::DegreesToRadians(FRotator::NormalizeAxis(View.Pitch));
		FCubeInput Input;
		if (bMouseCaptured)
		{
			Input.Forward = FMath::Clamp(AxisForward + TestForward, -1.f, 1.f);
			Input.Strafe = -FMath::Clamp(AxisRight, -1.f, 1.f);   // Minecraft's strafe is positive to the left
			Input.bJump = bJumpHeld; Input.bSneak = bSneakHeld; Input.bSprint = bSprintHeld;
		}
		Input.Yaw = Yaw;
		TArray<FCubeOtherBody> Others;
		for (const auto& Pair : Avatars) if (Pair.Value) { const FVector P = Pair.Value->GetActorLocation() / CubeSpec::BlockCm; Others.Add({ P.X, P.Y, P.Z, Pair.Value->bSneaking ? CubeSpec::SneakHeight : CubeSpec::Height }); }
		CubePhysics::PushAway(Body, Others);
		const FCubeSolidQuery Solid = [this](int32 X, int32 Y, int32 Z) { return Game->World.IsSolidForPhysics(X, Y, Z); };
		CubePhysics::Tick(Body, Input, Solid);
		UpdateAim();
		DigTick();
		SendMove(Yaw, Pitch);
	}
	Game->MaybeCross(Body.X);
}

void ACubePlayerPawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!Game) return;
	Accumulator += FMath::Min(DeltaSeconds, 0.25f);
	while (Accumulator >= CubeSpec::TickSeconds) { GameTick(); Accumulator -= CubeSpec::TickSeconds; }
	const double Partial = Accumulator / CubeSpec::TickSeconds;

	const FVector Feet(Body.PX + (Body.X - Body.PX) * Partial, Body.PY + (Body.Y - Body.PY) * Partial, Body.PZ + (Body.Z - Body.PZ) * Partial);
	SetActorLocation(Feet * CubeSpec::BlockCm);
	Camera->SetRelativeLocation(FVector(0, 0, Body.EyeHeight() * CubeSpec::BlockCm));

	const float TargetFov = CubeSpec::Fov * (Body.bSprinting ? CubeSpec::SprintFov : 1.f);
	Fov += (TargetFov - Fov) * FMath::Min(1.f, DeltaSeconds * 12.f);
	Camera->SetFieldOfView(Fov);

	if (bMouseCaptured && !Game->bDead) UpdateAim(); else Aim = FCubeAim();
	if (Aim.bBlock)
	{
		const FVector Center = (FVector(Aim.Hit.Block) + FVector(0.5)) * CubeSpec::BlockCm;
		DrawDebugBox(GetWorld(), Center, FVector(CubeSpec::BlockCm * 0.502f), FColor(0, 0, 0, 160), false, -1.f, 0, 1.5f);
	}
}
