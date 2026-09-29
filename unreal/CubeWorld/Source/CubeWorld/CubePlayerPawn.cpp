#include "CubePlayerPawn.h"
#include "CubeWorld.h"
#include "CubeWorldGameInstance.h"
#include "CubeWorldGameMode.h"
#include "CubeAvatar.h"
#include "CubeWorldActor.h"
#include "CubeBombs.h"
#include "CubeTombstone.h"
#include "ProceduralMeshComponent.h"
#include "Camera/CameraComponent.h"
#include "Engine/GameViewportClient.h"
#include "Engine/Engine.h"
#include "Components/InputComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "Kismet/GameplayStatics.h"
#include "DrawDebugHelpers.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "EngineUtils.h"
#include "Net/UnrealNetwork.h"
#include "Dom/JsonObject.h"
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
	// The pawn is the player's own channel to the server: nobody else needs it, others see the presence list.
	bReplicates = true;
	bOnlyRelevantToOwner = true;
	bNetLoadOnClient = false;
	bReplicateUsingRegisteredSubObjectList = true;
	SetReplicatingMovement(false);
}

void ACubePlayerPawn::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME_CONDITION(ACubePlayerPawn, PlayerId, COND_OwnerOnly);
}

void ACubePlayerPawn::OnRep_PlayerId()
{
	if (Game && !PlayerId.IsEmpty()) Game->PlayerId = PlayerId;
}

void ACubePlayerPawn::BeginPlay()
{
	Super::BeginPlay();
	if (GetNetMode() == NM_DedicatedServer || CubeIsServerProcess()) return;
	Game = Cast<UCubeWorldGameInstance>(GetGameInstance());
}

// The local player's pawn binds once it is possessed: on a network client the controller arrives after BeginPlay.
void ACubePlayerPawn::Bind()
{
	if (!Game || bBound || !IsLocallyControlled()) return;
	SkinMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Materials/M_Skin.M_Skin"));
	if (!SkinMaterial) SkinMaterial = UMaterial::GetDefaultMaterial(MD_Surface);

	Game->OnWelcome.AddUObject(this, &ACubePlayerPawn::HandleWelcome);
	Game->OnRespawn.AddUObject(this, &ACubePlayerPawn::HandleRespawn);
	Game->OnHurt.AddUObject(this, &ACubePlayerPawn::HandleHurt);
	Game->OnDeath.AddUObject(this, &ACubePlayerPawn::HandleDeath);
	Game->OnPlayers.AddUObject(this, &ACubePlayerPawn::HandlePlayers);
	Game->OnCube.AddUObject(this, &ACubePlayerPawn::HandleCube);
	Game->OnBomb.AddUObject(this, &ACubePlayerPawn::HandleBomb);
	bBound = true;
	OnRep_PlayerId();

	// After a border crossing the player stands where they were, in the world the client already holds, and keeps
	// walking while the new server's welcome is on its way: no start screen, no jump to the region's middle.
	const bool bCrossingIn = Game->Crossing.bSet;
	if (bCrossingIn)
	{
		const FCubeCrossing& C = Game->Crossing;
		Body.Teleport(C.X, C.Y, C.Z);
		Body.VX = C.VX; Body.VY = C.VY; Body.VZ = C.VZ;
	}
	else Body.Teleport(36, 12, 0);
	Game->bPlaced = bCrossingIn;
	if (APlayerController* PC = Cast<APlayerController>(GetController()))
	{
		if (!bCrossingIn) PC->SetControlRotation(FRotator(-15.f, 135.f, 0));
		PC->PlayerCameraManager->ViewPitchMin = -89.9f;
		PC->PlayerCameraManager->ViewPitchMax = 89.9f;
	}
	CaptureMouse(bCrossingIn);
	// The players seen a moment ago stand where they were until the next server's list takes over.
	if (bCrossingIn) HandlePlayers(Game->Players);
	// The pawn stands at the crossing pose: the view the crossing camera held is its own from here.
	if (bCrossingIn)
	{
		SetActorLocation(FVector(Body.X, Body.Y, Body.Z) * CubeSpec::BlockCm);
		if (APlayerController* View = Cast<APlayerController>(GetController()))
			View->SetControlRotation(FRotator(-FMath::RadiansToDegrees(Game->Crossing.Pitch), FMath::RadiansToDegrees(Game->Crossing.Yaw) + 90.f, 0));
		Game->EndCrossingView();
	}

	if (Game->PlayerName.IsEmpty())
	{
		FString Name;
		if (!FParse::Value(FCommandLine::Get(), TEXT("-name="), Name) || Name.IsEmpty())
			Name = FString::Printf(TEXT("Player-%04d"), FMath::RandRange(0, 9999));
		Game->PlayerName = Name;
	}

	// On a server: say who we are and, after a border crossing, where we were.
	if (GetNetMode() == NM_Client)
	{
		const FCubeCrossing& C = Game->Crossing;
		APlayerController* View = Cast<APlayerController>(GetController());
		if (C.bSet && View) View->SetControlRotation(FRotator(-FMath::RadiansToDegrees(C.Pitch), FMath::RadiansToDegrees(C.Yaw) + 90.f, 0));
		ServerHello(Game->PlayerName, C.bSet, C.X, C.Y, C.Z);
		return;
	}
	SetupUnattended();
}

// Unattended runs from the menu map: -autoplay signs in at once, -screenshot=<s> saves Saved/Screenshots/cube.png
// after that many seconds, -quitafter=<s> ends the process, -selftest walks, places, digs and hits once in.
void ACubePlayerPawn::SetupUnattended()
{
	if (FParse::Param(FCommandLine::Get(), TEXT("autoplay")))
	{
		FTimerHandle Handle;
		GetWorldTimerManager().SetTimer(Handle, [this]() { if (Game) Game->StartPlay(Game->PlayerName); }, 1.f, false);
	}
	float Seconds = 0;
	if (FParse::Value(FCommandLine::Get(), TEXT("-screenshot="), Seconds) && Seconds > 0)
	{
		FTimerHandle Handle;
		Game->GetTimerManager().SetTimer(Handle, []() { FScreenshotRequest::RequestScreenshot(TEXT("cube.png"), false, false); }, Seconds, false);
	}
	if (FParse::Value(FCommandLine::Get(), TEXT("-quitafter="), Seconds) && Seconds > 0)
	{
		FTimerHandle Handle;
		Game->GetTimerManager().SetTimer(Handle, []() { FPlatformMisc::RequestExit(false); }, Seconds, false);
	}
}

ACubeWorldActor* ACubePlayerPawn::WorldActor() const
{
	TActorIterator<ACubeWorldActor> It(GetWorld());
	return It ? *It : nullptr;
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

// The axis value is the raw pixel delta (DefaultInput.ini sets the mouse sensitivity to 1 and the legacy scales off).
void ACubePlayerPawn::OnTurn(float V) { if (bMouseCaptured) AddControllerYawInput(V * CubeSpec::DegreesPerMousePixel); }
void ACubePlayerPawn::OnLookUp(float V) { if (bMouseCaptured) AddControllerPitchInput(V * CubeSpec::DegreesPerMousePixel); }

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
	if (Game->bDead) { CmdRespawn(); return; }
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
	if (Aim.bPlayer) CmdAttack(Aim.PlayerId);
}

void ACubePlayerPawn::OnPlace()
{
	if (!Game || !bMouseCaptured || Game->bDead) return;
	if (!Game->Holding.IsEmpty()) { ThrowBomb(); return; }
	UpdateAim();
	if (!Aim.bBlock) return;
	const FName Kind = Game->Hotbar.IsValidIndex(Slot()) ? Game->Hotbar[Slot()] : NAME_None;
	if (Kind == NAME_None || Game->Inventory.FindRef(Kind) <= 0) return;
	CmdPlace(Aim.Hit.Block.X, Aim.Hit.Block.Y, Aim.Hit.Block.Z, Aim.Hit.Normal.X, Aim.Hit.Normal.Y, Aim.Hit.Normal.Z, Kind);
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

void ACubePlayerPawn::HandleWelcome(const FCubePose& You, bool)
{
	// A crossing keeps the body where the client has walked it meanwhile; the server's pose is where the crossing began.
	const bool bKeep = Game->bPlaced && FMath::Abs(You.X - Body.X) + FMath::Abs(You.Y - Body.Y) + FMath::Abs(You.Z - Body.Z) < 12;
	if (bKeep) Unstick();
	else
	{
		Spawn(You.X, You.Y, You.Z);
		LastPose.Empty();
		for (auto& Pair : Avatars) if (Pair.Value) Pair.Value->Destroy();
		Avatars.Empty();
		TArray<FString> Ids;
		Bombs.GetKeys(Ids);
		for (const FString& Id : Ids) RemoveBomb(Id);
	}
	if (!bMouseCaptured) CaptureMouse(true);

	// -selftest: once in, walk forward for a second, place a block ahead, dig it back, hit whoever is near.
	static bool bTested = false;
	if (FParse::Param(FCommandLine::Get(), TEXT("selftest")) && !bTested)
	{
		bTested = true;
		FTimerHandle H1, H2, H3, H4, H5;
		GetWorldTimerManager().SetTimer(H1, FTimerDelegate::CreateWeakLambda(this, [this]() { bMouseCaptured = true; TestForward = 1.f; Game->Log(FString::Printf(TEXT("selftest: walking from %.2f %.2f"), Body.X, Body.Y)); }), 3.f, false);
		GetWorldTimerManager().SetTimer(H2, FTimerDelegate::CreateWeakLambda(this, [this]() { TestForward = 0.f; Game->Log(FString::Printf(TEXT("selftest: stopped at %.2f %.2f (ground %d)"), Body.X, Body.Y, Body.bOnGround)); }), 4.f, false);
		GetWorldTimerManager().SetTimer(H3, FTimerDelegate::CreateWeakLambda(this, [this]()
		{
			// On top of the highest block two columns ahead, wherever the ground is (the world may hold craters).
			const int32 X = FMath::FloorToInt32(Body.X) + 2, Y = FMath::FloorToInt32(Body.Y);
			int32 Z = FMath::FloorToInt32(Body.Z) + 1;
			while (Z > CubeSpec::MinZ && !Game->World.IsSolid(X, Y, Z)) Z--;
			TestPlaced = FIntVector(X, Y, Z + 1);
			CmdPlace(X, Y, Z, 0, 0, 1, TEXT("gold"));
			Game->Log(FString::Printf(TEXT("selftest: placed gold at %d %d %d"), X, Y, Z + 1));
		}), 5.f, false);
		GetWorldTimerManager().SetTimer(H4, FTimerDelegate::CreateWeakLambda(this, [this]()
		{
			const FIntVector& P = TestPlaced;
			Game->Log(FString::Printf(TEXT("selftest: world says %s at %d %d %d, gold left %d"), *Game->World.KindAt(P.X, P.Y, P.Z).ToString(), P.X, P.Y, P.Z, Game->Inventory.FindRef(TEXT("gold"))));
			TestDug = FIntVector(FMath::FloorToInt32(Body.X), FMath::FloorToInt32(Body.Y), FMath::FloorToInt32(Body.Z) - 1);
			CmdDig(TestDug.X, TestDug.Y, TestDug.Z, true);
			Game->Log(FString::Printf(TEXT("selftest: digging %s under the feet at %d %d %d"), *Game->World.KindAt(TestDug.X, TestDug.Y, TestDug.Z).ToString(), TestDug.X, TestDug.Y, TestDug.Z));
		}), 6.f, false);
		GetWorldTimerManager().SetTimer(H5, FTimerDelegate::CreateWeakLambda(this, [this]()
		{
			const FIntVector& D = TestDug;
			Game->Log(FString::Printf(TEXT("selftest: after dig %s at %d %d %d, dirt %d, players seen %d"), *Game->World.KindAt(D.X, D.Y, D.Z).ToString(), D.X, D.Y, D.Z, Game->Inventory.FindRef(TEXT("dirt")), Game->Players.Num()));
			for (const FCubePresence& P : Game->Players) if (P.Id != Game->PlayerId) { CmdAttack(P.Id); Game->Log(FString::Printf(TEXT("selftest: hit %s"), *P.Name)); }
		}), 9.f, false);
		Game->OnCube.AddLambda([this](int32 X, int32 Y, int32 Z, FName Kind) { Game->Log(FString::Printf(TEXT("cube %d %d %d -> %s"), X, Y, Z, *Kind.ToString())); });
		Game->OnDig.AddLambda([this](const FString&, int32 X, int32 Y, int32 Z, int32 Stage) { if (Stage <= 0 || Stage == 9) Game->Log(FString::Printf(TEXT("dig %d %d %d stage %d"), X, Y, Z, Stage)); });
	}
	// -bombtest: walk onto the first free bomb, throw it ahead, and say what became of it.
	if (FParse::Param(FCommandLine::Get(), TEXT("bombtest")))
	{
		static int32 Phase = 0;
		static FString TestBomb;
		FTimerHandle HB;
		GetWorldTimerManager().SetTimer(HB, FTimerDelegate::CreateWeakLambda(this, [this]()
		{
			if (Phase == 0)
			{
				for (const auto& Pair : Bombs)
					if (Pair.Value && Pair.Value->State == TEXT("free"))
					{
						TestBomb = Pair.Key;
						const FVector P = Pair.Value->Pos;
						Body.Teleport(P.X, P.Y, FMath::Max(0.0, P.Z));
						Game->Log(FString::Printf(TEXT("bombtest: standing on bomb %s at %.1f %.1f %.1f"), *TestBomb, P.X, P.Y, P.Z));
						Phase = 1;
						return;
					}
				Game->Log(FString::Printf(TEXT("bombtest: no free bomb yet (%d bombs known)"), Bombs.Num()));
			}
			else if (Phase == 1 && !Game->Holding.IsEmpty())
			{
				Game->Log(FString::Printf(TEXT("bombtest: holding %s, throwing"), *Game->Holding));
				CmdThrow(FVector(0.9, 0, 0.44));
				Phase = 2;
			}
			else if (Phase == 2)
			{
				if (ACubeBomb** B = Bombs.Find(TestBomb))
					if (*B) Game->Log(FString::Printf(TEXT("bombtest: %s at %.2f %.2f %.2f stopped %lld"), *(*B)->State, (*B)->Pos.X, (*B)->Pos.Y, (*B)->Pos.Z, (*B)->Stopped));
			}
		}), 0.5f, true, 6.f);
	}
	// -walkto=<x> or -walkto=<x>,<y>: keep walking towards that spot, one axis at a time (a border crossing test).
	FString TargetText;
	if (FParse::Value(FCommandLine::Get(), TEXT("-walkto="), TargetText, false) && !TargetText.IsEmpty())
	{
		FString TargetX, TargetY;
		if (!TargetText.Split(TEXT(","), &TargetX, &TargetY)) TargetX = TargetText;
		const float Target = FCString::Atof(*TargetX);
		const TOptional<float> TargetYValue = TargetY.IsEmpty() ? TOptional<float>() : TOptional<float>(FCString::Atof(*TargetY));
		FTimerHandle H;
		GetWorldTimerManager().SetTimer(H, FTimerDelegate::CreateWeakLambda(this, [this, Target, TargetYValue]()
		{
			TestWalkTo = Target;
			TestWalkToY = TargetYValue;
			bMouseCaptured = true; TestForward = 1.f; bSprintHeld = true;
		}), 2.f, false);
		FTimerHandle Where;
		GetWorldTimerManager().SetTimer(Where, FTimerDelegate::CreateWeakLambda(this, [this]() { Game->Log(FString::Printf(TEXT("walkto: at %.1f %.1f %.1f yaw %.0f in %s"), Body.X, Body.Y, Body.Z, GetControlRotation().Yaw, *Game->Room)); }), 1.f, true);
	}
}

void ACubePlayerPawn::HandleRespawn(const FCubePose& You)
{
	Spawn(You.X, You.Y, You.Z);
	CaptureMouse(true);
}

void ACubePlayerPawn::HandleHurt(const FString& InPlayerId, double, double KX, double KY, double Strength)
{
	if (InPlayerId == Game->PlayerId)
	{
		HurtUntil = FPlatformTime::Seconds() + CubeSpec::HurtTicks * CubeSpec::TickSeconds;
		if (Strength > 0) CubePhysics::Knockback(Body, KX, KY, Strength);
		return;
	}
	if (ACubeAvatar** A = Avatars.Find(InPlayerId)) if (*A) (*A)->Hurt();
}

void ACubePlayerPawn::HandleDeath(const FString& InPlayerId, const FString& By)
{
	if (InPlayerId == Game->PlayerId) { CaptureMouse(false); bDigging = false; }
	FString Who = InPlayerId == Game->PlayerId ? TEXT("you") : InPlayerId, Killer = By;
	for (const FCubePresence& P : Game->Players) { if (P.Id == InPlayerId) Who = P.Name; if (P.Id == By) Killer = P.Name; }
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
	// A player missing from the list is most often walking over a border: the next server lists them a second or two
	// later. They walk on as they were going meanwhile, and only one gone for 4 s leaves.
	for (auto It = Avatars.CreateIterator(); It; ++It)
	{
		if (Seen.Contains(It.Key()) || !It.Value()) { if (!It.Value()) It.RemoveCurrent(); continue; }
		if (It.Value()->MissingFor() > 4.0) { It.Value()->Destroy(); It.RemoveCurrent(); }
		else It.Value()->MarkMissing();
	}
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
		if (!A || A->IsDead()) continue;
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
	if (bDigging) CmdDig(DigTarget.X, DigTarget.Y, DigTarget.Z, false);
	bDigging = bWant;
	DigTarget = Target;
	if (bWant) CmdDig(Target.X, Target.Y, Target.Z, true);
}

void ACubePlayerPawn::SendMove(double Yaw, double Pitch)
{
	const FString Pose = FString::Printf(TEXT("%.3f,%.3f,%.3f,%.3f,%.3f,%d,%d,%d"), Body.X, Body.Y, Body.Z, Yaw, Pitch, Body.bOnGround, Body.bSneaking, Body.bSprinting);
	if (Pose == LastPose) return;
	LastPose = Pose;
	CmdMove(Body.X, Body.Y, Body.Z, Yaw, Pitch, Body.bOnGround, Body.bSneaking, Body.bSprinting);
}

void ACubePlayerPawn::GameTick()
{
	TickCount++;
	TArray<ACubeBomb*> Live;
	Bombs.GenerateValueArray(Live);
	for (ACubeBomb* Bomb : Live) if (Bomb) TickBomb(Bomb);
	if (!Game->bPlaced) return;
	// A -walkto test steers itself every tick: two client windows on one desktop fight over the mouse.
	if (TestWalkTo.IsSet()) if (APlayerController* PC = Cast<APlayerController>(GetController()))
	{
		// Along x until there, then along y (Unreal's yaw 90 is the world's +y).
		const bool bThereX = FMath::Abs(TestWalkTo.GetValue() - Body.X) < 0.5;
		if (bThereX && TestWalkToY.IsSet()) PC->SetControlRotation(FRotator(0, TestWalkToY.GetValue() > Body.Y ? 90.f : -90.f, 0));
		else PC->SetControlRotation(FRotator(0, TestWalkTo.GetValue() > Body.X ? 0.f : 180.f, 0));
	}
	const FRotator View = GetControlRotation();
	const double Yaw = CubeSpec::YawFromUnreal(View.Yaw), Pitch = CubeSpec::PitchFromUnreal(View.Pitch);
	if (!Game->bDead)
	{
		FCubeInput Input;
		if (bMouseCaptured)
		{
			Input.Forward = FMath::Clamp(AxisForward + TestForward, -1.f, 1.f);
			Input.Strafe = -FMath::Clamp(AxisRight, -1.f, 1.f);   // Minecraft's strafe is positive to the left
			Input.bJump = bJumpHeld; Input.bSneak = bSneakHeld; Input.bSprint = bSprintHeld;
		}
		Input.Yaw = Yaw;
		TArray<FCubeOtherBody> Others;
		for (const auto& Pair : Avatars) if (Pair.Value && !Pair.Value->IsDead()) { const FVector P = Pair.Value->GetActorLocation() / CubeSpec::BlockCm; Others.Add({ P.X, P.Y, P.Z, Pair.Value->bSneaking ? CubeSpec::SneakHeight : CubeSpec::Height }); }
		CubePhysics::PushAway(Body, Others);
		const FCubeSolidQuery Solid = [this](int32 X, int32 Y, int32 Z) { return Game->World.IsSolidForPhysics(X, Y, Z); };
		CubePhysics::Tick(Body, Input, Solid);
		UpdateAim();
		DigTick();
		SendMove(Yaw, Pitch);
	}
	Game->LastBody = { true, Body.X, Body.Y, Body.Z, Yaw, Pitch, Body.VX, Body.VY, Body.VZ };
	Game->MaybeCross(Body.X, Body.Y);
}

void ACubePlayerPawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!bBound) Bind();
	if (!Game || !bBound) return;
	Accumulator += FMath::Min(DeltaSeconds, 0.25f);
	while (Accumulator >= CubeSpec::TickSeconds) { GameTick(); Accumulator -= CubeSpec::TickSeconds; }
	const double Partial = Accumulator / CubeSpec::TickSeconds;

	const FVector Feet(Body.PX + (Body.X - Body.PX) * Partial, Body.PY + (Body.Y - Body.PY) * Partial, Body.PZ + (Body.Z - Body.PZ) * Partial);
	SetActorLocation(Feet * CubeSpec::BlockCm);
	Camera->SetRelativeLocation(FVector(0, 0, Body.EyeHeight() * CubeSpec::BlockCm));

	// Minecraft's angle is vertical; Unreal wants the horizontal one for the viewport's aspect.
	const float TargetFov = CubeSpec::Fov * (Body.bSprinting ? CubeSpec::SprintFov : 1.f);
	Fov += (TargetFov - Fov) * FMath::Min(1.f, DeltaSeconds * 12.f);
	float Aspect = 16.f / 9.f;
	if (GEngine && GEngine->GameViewport) { FVector2D Size; GEngine->GameViewport->GetViewportSize(Size); if (Size.Y > 0) Aspect = Size.X / Size.Y; }
	const float Horizontal = FMath::RadiansToDegrees(2.f * FMath::Atan(FMath::Tan(FMath::DegreesToRadians(Fov / 2.f)) * Aspect));
	Camera->SetFieldOfView(Horizontal);

	const double Now = FPlatformTime::Seconds();
	for (const auto& Pair : Bombs)
	{
		ACubeBomb* Bomb = Pair.Value;
		if (!Bomb) continue;
		if (Bomb->State == TEXT("held")) PlaceHeld(Bomb);
		else Bomb->SetActorLocation(FMath::Lerp(Bomb->Prev, Bomb->Pos, Partial) * CubeSpec::BlockCm);
		Bomb->Animate(Now);
	}
	ShowMyTomb();

	if (bMouseCaptured && !Game->bDead) UpdateAim(); else Aim = FCubeAim();
	if (Aim.bBlock)
	{
		const FVector Center = (FVector(Aim.Hit.Block) + FVector(0.5)) * CubeSpec::BlockCm;
		DrawDebugBox(GetWorld(), Center, FVector(CubeSpec::BlockCm * 0.502f), FColor(0, 0, 0, 160), false, -1.f, 0, 1.5f);
	}
}

// ── to the server ────────────────────────────────────────────────────────────────────────────────

namespace
{
	ACubeWorldGameMode* ServerOf(const AActor* Actor)
	{
		return Actor && Actor->HasAuthority() ? Cast<ACubeWorldGameMode>(Actor->GetWorld()->GetAuthGameMode()) : nullptr;
	}
}

void ACubePlayerPawn::ServerHello_Implementation(const FString& Name, bool bCross, float X, float Y, float Z)
{
	if (ACubeWorldGameMode* S = ServerOf(this)) S->OnHello(this, Name, bCross, X, Y, Z);
}

void ACubePlayerPawn::ServerMove_Implementation(float X, float Y, float Z, float Yaw, float Pitch, bool bOnGround, bool bSneaking, bool bSprinting)
{
	if (ACubeWorldGameMode* S = ServerOf(this)) S->OnMove(S->PlayerOf(this), X, Y, Z, Yaw, Pitch, bOnGround, bSneaking, bSprinting);
}

void ACubePlayerPawn::ServerDig_Implementation(int32 X, int32 Y, int32 Z, bool bStart)
{
	if (ACubeWorldGameMode* S = ServerOf(this)) S->OnDig(S->PlayerOf(this), X, Y, Z, bStart);
}

void ACubePlayerPawn::ServerPlace_Implementation(int32 X, int32 Y, int32 Z, int32 NX, int32 NY, int32 NZ, uint8 Kind)
{
	if (ACubeWorldGameMode* S = ServerOf(this)) S->OnPlace(S->PlayerOf(this), X, Y, Z, NX, NY, NZ, CubeSpec::KindOf(Kind));
}

void ACubePlayerPawn::ServerAttack_Implementation(const FString& Target)
{
	if (ACubeWorldGameMode* S = ServerOf(this)) S->OnAttack(S->PlayerOf(this), Target);
}

void ACubePlayerPawn::ServerRespawn_Implementation()
{
	if (ACubeWorldGameMode* S = ServerOf(this)) S->OnRespawn(S->PlayerOf(this));
}

void ACubePlayerPawn::ServerThrow_Implementation(float DX, float DY, float DZ)
{
	if (ACubeWorldGameMode* S = ServerOf(this)) S->OnThrow(S->PlayerOf(this), DX, DY, DZ);
}

// ── from the server ──────────────────────────────────────────────────────────────────────────────

void ACubePlayerPawn::ClientWelcome_Implementation(const FCubeWelcomeRep& W, const TArray<FCubeStackRep>& Stacks)
{
	if (!Game) return;
	Game->OnWelcomed(W.Server, W.Color, W.Room, W.Region, FCubePose{ W.X, W.Y, W.Z, W.Health }, Stacks, W.Chunks);
}

void ACubePlayerPawn::ClientWorldChunk_Implementation(const TArray<FCubeCellRep>& Cells, bool bLast)
{
	if (Game) Game->OnWorldChunk(Cells, bLast);
}

void ACubePlayerPawn::ClientBombs_Implementation(const TArray<FCubeBombRep>& InBombs)
{
	if (!Game) return;
	for (const FCubeBombRep& B : InBombs) Game->OnBombFrame(B);
}

void ACubePlayerPawn::ClientInventory_Implementation(const TArray<FCubeStackRep>& Stacks)
{
	if (Game) Game->SetInventory(Stacks);
}

void ACubePlayerPawn::ClientRespawn_Implementation(float X, float Y, float Z)
{
	if (Game) Game->OnRespawnFrame(FCubePose{ X, Y, Z, CubeSpec::MaxHealth });
}

void ACubePlayerPawn::ClientTurnedAway_Implementation(const FString& Reason)
{
	if (Game) Game->TurnedAwayBy(Reason);
}

// ── bombs ────────────────────────────────────────────────────────────────────────────────────────
// A free bomb comes down under its parachute, a held one sits in its holder's hand, a thrown one flies the
// path the server flies it. The server says when one is picked up, thrown, explodes or fizzles out.

void ACubePlayerPawn::HandleBomb(const FCubeBombFrame& B)
{
	ACubeBomb** Found = Bombs.Find(B.Id);
	ACubeBomb* Bomb = Found ? *Found : nullptr;
	const FVector At(B.X, B.Y, B.Z);
	if (B.State == TEXT("exploded"))
	{
		ACubeBurst::Explosion(GetWorld(), At);
		Game->Log(FString::Printf(TEXT("%s blew up a bomb"), *Game->NameOf(B.Holder)));
		RemoveBomb(B.Id);
		return;
	}
	if (B.State == TEXT("fizzled"))
	{
		if (Bomb || B.Age < 5000) ACubeBurst::Smoke(GetWorld(), Bomb ? Bomb->GetActorLocation() / CubeSpec::BlockCm : FVector(B.X, B.Y, B.Height.Get(B.Z)));
		RemoveBomb(B.Id);
		return;
	}
	if (!Bomb)
	{
		Bomb = GetWorld()->SpawnActor<ACubeBomb>();
		if (!Bomb) return;
		Bomb->Id = B.Id;
		Bombs.Add(B.Id, Bomb);
	}
	Bomb->State = B.State;
	Bomb->Holder = B.Holder;
	Bomb->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
	Bomb->SetActorRotation(FRotator::ZeroRotator);
	Bomb->SetActorScale3D(FVector(1));
	Bomb->SetActorHiddenInGame(false);
	if (B.State == TEXT("free"))
	{
		Bomb->Pos = FVector(B.X, B.Y, B.Height.Get(B.Z));
		// Already resting on a block: the parachute stays folded, whatever frame repeats the record.
		Bomb->bLanded = CubeBombs::Descend(Game->World, Bomb->Pos.X, Bomb->Pos.Y, Bomb->Pos.Z) == Bomb->Pos.Z;
	}
	else if (B.State == TEXT("flying"))
	{
		// Heard late, it is flown as far as it has come.
		Bomb->Pos = At;
		Bomb->Vel = FVector(B.VX, B.VY, B.VZ);
		Bomb->Stopped = 0;
		const int32 Ticks = FMath::Min(CubeSpec::BombFlightTicks, FMath::FloorToInt32(B.Age / (CubeSpec::TickSeconds * 1000)));
		for (int32 T = 0; T < Ticks && !Bomb->Stopped; T++) TickBomb(Bomb);
	}
	if (B.State != TEXT("free") && Bomb->Parachute) Bomb->Parachute->SetVisibility(false);
	Bomb->Prev = Bomb->Pos;
	if (B.State != TEXT("held")) Bomb->SetActorLocation(Bomb->Pos * CubeSpec::BlockCm);
	UpdateHolding();
}

void ACubePlayerPawn::RemoveBomb(const FString& Id)
{
	ACubeBomb* Bomb = nullptr;
	if (!Bombs.RemoveAndCopyValue(Id, Bomb)) return;
	if (Bomb) Bomb->Destroy();
	UpdateHolding();
}

void ACubePlayerPawn::UpdateHolding()
{
	Game->Holding.Empty();
	for (const auto& Pair : Bombs)
		if (Pair.Value && Pair.Value->State == TEXT("held") && Pair.Value->Holder == Game->PlayerId) Game->Holding = Pair.Key;
}

void ACubePlayerPawn::TickBomb(ACubeBomb* Bomb)
{
	Bomb->Prev = Bomb->Pos;
	if (Bomb->State == TEXT("free"))
	{
		Bomb->Pos.Z = CubeBombs::Descend(Game->World, Bomb->Pos.X, Bomb->Pos.Y, Bomb->Pos.Z);
		Bomb->bLanded = Bomb->Pos.Z == Bomb->Prev.Z;
	}
	else if (Bomb->State == TEXT("flying") && !Bomb->Stopped)
	{
		if (CubeBombs::Fly(Game->World, Bomb->Pos, Bomb->Vel) != ECubeFlight::Flying) Bomb->Stopped = FMath::Max<int64>(1, TickCount);
	}
	// The server's word on where it went off comes soon; one that never comes is taken away after three seconds.
	else if (Bomb->State == TEXT("flying") && TickCount - Bomb->Stopped > 60)
	{
		RemoveBomb(Bomb->Id);
	}
}

// Where a held bomb goes: in front of the camera for the holder, in the right hand of anyone else's model.
void ACubePlayerPawn::PlaceHeld(ACubeBomb* Bomb)
{
	USceneComponent* Hand = nullptr;
	if (Bomb->Holder == Game->PlayerId) Hand = Camera;
	else if (ACubeAvatar** A = Avatars.Find(Bomb->Holder)) if (*A && !(*A)->IsDead()) Hand = (*A)->Hand();
	USceneComponent* Root = Bomb->GetRootComponent();
	if (!Hand)
	{
		if (Root->GetAttachParent()) Bomb->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
		if (!Bomb->IsHidden()) Bomb->SetActorHiddenInGame(true);
		return;
	}
	if (Root->GetAttachParent() == Hand) return;
	Bomb->SetActorHiddenInGame(false);
	Bomb->AttachToComponent(Hand, FAttachmentTransformRules::KeepRelativeTransform);
	const bool bOwn = Hand == Camera;
	Root->SetRelativeLocationAndRotation(bOwn ? FVector(50, 22, -22) : FVector(0, 0, -95), FRotator::ZeroRotator);
	Root->SetRelativeScale3D(FVector(bOwn ? 0.32 : 1));
}

void ACubePlayerPawn::ThrowBomb()
{
	const FVector Look = Camera->GetForwardVector();
	CmdThrow(Look);
}

// A dead player leaves the map, you too: your tombstone stands where you fell until you respawn.
void ACubePlayerPawn::ShowMyTomb()
{
	if (Game->bDead && !MyTomb)
	{
		MyTomb = GetWorld()->SpawnActor<ACubeTombstone>();
		if (MyTomb) MyTomb->Setup(Game->PlayerName);
	}
	if (!MyTomb) return;
	if (!Game->bDead) MyTomb->Show(false);
	else if (MyTomb->IsHidden()) MyTomb->Show(true, FVector(Body.X, Body.Y, Body.Z), FMath::DegreesToRadians(GetControlRotation().Yaw - 90.f));
}

// ── the same requests, by whichever door the server is behind ────────────────────────────────────

namespace
{
	TSharedRef<FJsonObject> Op(const TCHAR* Name)
	{
		TSharedRef<FJsonObject> F = MakeShared<FJsonObject>();
		F->SetStringField(TEXT("op"), Name);
		return F;
	}
}

void ACubePlayerPawn::CmdMove(double X, double Y, double Z, double Yaw, double Pitch, bool bOnGround, bool bSneaking, bool bSprinting)
{
	if (!Game->IsViaSocket()) { ServerMove(X, Y, Z, Yaw, Pitch, bOnGround, bSneaking, bSprinting); return; }
	const TSharedRef<FJsonObject> F = Op(TEXT("move"));
	F->SetNumberField(TEXT("x"), X); F->SetNumberField(TEXT("y"), Y); F->SetNumberField(TEXT("z"), Z);
	F->SetNumberField(TEXT("yaw"), Yaw); F->SetNumberField(TEXT("pitch"), Pitch);
	F->SetBoolField(TEXT("onGround"), bOnGround); F->SetBoolField(TEXT("sneaking"), bSneaking); F->SetBoolField(TEXT("sprinting"), bSprinting);
	Game->Send(F);
}

void ACubePlayerPawn::CmdDig(int32 X, int32 Y, int32 Z, bool bStart)
{
	if (!Game->IsViaSocket()) { ServerDig(X, Y, Z, bStart); return; }
	const TSharedRef<FJsonObject> F = Op(TEXT("dig"));
	F->SetStringField(TEXT("state"), bStart ? TEXT("start") : TEXT("stop"));
	F->SetNumberField(TEXT("x"), X); F->SetNumberField(TEXT("y"), Y); F->SetNumberField(TEXT("z"), Z);
	Game->Send(F);
}

void ACubePlayerPawn::CmdPlace(int32 X, int32 Y, int32 Z, int32 NX, int32 NY, int32 NZ, FName Kind)
{
	if (!Game->IsViaSocket()) { ServerPlace(X, Y, Z, NX, NY, NZ, CubeSpec::KindIndex(Kind)); return; }
	const TSharedRef<FJsonObject> F = Op(TEXT("place"));
	F->SetNumberField(TEXT("x"), X); F->SetNumberField(TEXT("y"), Y); F->SetNumberField(TEXT("z"), Z);
	F->SetNumberField(TEXT("nx"), NX); F->SetNumberField(TEXT("ny"), NY); F->SetNumberField(TEXT("nz"), NZ);
	F->SetStringField(TEXT("kind"), Kind.ToString());
	Game->Send(F);
}

void ACubePlayerPawn::CmdAttack(const FString& Target)
{
	if (!Game->IsViaSocket()) { ServerAttack(Target); return; }
	const TSharedRef<FJsonObject> F = Op(TEXT("attack"));
	F->SetStringField(TEXT("target"), Target);
	Game->Send(F);
}

void ACubePlayerPawn::CmdRespawn()
{
	if (!Game->IsViaSocket()) { ServerRespawn(); return; }
	Game->Send(Op(TEXT("respawn")));
}

void ACubePlayerPawn::CmdThrow(const FVector& Direction)
{
	if (!Game->IsViaSocket()) { ServerThrow(Direction.X, Direction.Y, Direction.Z); return; }
	const TSharedRef<FJsonObject> F = Op(TEXT("throw"));
	F->SetNumberField(TEXT("x"), Direction.X); F->SetNumberField(TEXT("y"), Direction.Y); F->SetNumberField(TEXT("z"), Direction.Z);
	Game->Send(F);
}
