#include "CubeWorldGameInstance.h"
#include "CubeWorld.h"
#include "CubeWorldActor.h"
#include "CubeSocket.h"
#include "PlayServ.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "TimerManager.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "CubeHUD.h"
#include "CubeKeys.h"
#include "Misc/CoreDelegates.h"
#include "Misc/App.h"
#include "GameFramework/HUD.h"
#include "Engine/LocalPlayer.h"
#include "Engine/GameViewportClient.h"
#include "Camera/PlayerCameraManager.h"
#include "Containers/Ticker.h"
#include "CubeSpec.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	/** Where the guest playing under this name keeps their refresh token: their only credential, single-use, rotated about every 12 minutes. */
	FString GuestFile(const FString& Name)
	{
		FString Safe = Name.ToLower();
		for (TCHAR& C : Safe) if (!FChar::IsAlnum(C) && C != TEXT('-') && C != TEXT('_')) C = TEXT('_');
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Guests"), Safe + TEXT(".token"));
	}
}

void UCubeWorldGameInstance::Init()
{
	Super::Init();
	if (!CubeIsServerProcess())
	{
		Textures.Build();
		// The SDK hands the refresh token over at login and on every rotation; it is written at once, since a spent one
		// can never be used again.
		PlayServ::Auth::SetRefreshTokenChangedHandler(FPlayServRefreshTokenChanged::CreateUObject(this, &UCubeWorldGameInstance::KeepGuest));
		PlayServ::Auth::OnSessionLost().AddDynamic(this, &UCubeWorldGameInstance::HandleSessionLost);
	}
	Hotbar = CubeSpec::Hotbar();
	FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(this, &UCubeWorldGameInstance::HandlePostLoadMap);
	if (GEngine) GEngine->OnNetworkFailure().AddUObject(this, &UCubeWorldGameInstance::HandleNetworkFailure);
}

void UCubeWorldGameInstance::KeepGuest(const FString& RefreshToken)
{
	if (PlayerName.IsEmpty() || RefreshToken.IsEmpty()) return;
	if (!FFileHelper::SaveStringToFile(RefreshToken, *GuestFile(PlayerName))) Log(TEXT("the guest's sign-in could not be kept: the next launch makes a new one"));
}

// The platform refused the token: the guest it kept is gone, and the next sign-in under this name makes a new one.
void UCubeWorldGameInstance::HandleSessionLost()
{
	if (!PlayerName.IsEmpty()) IFileManager::Get().Delete(*GuestFile(PlayerName), false, true, true);
}

void UCubeWorldGameInstance::Shutdown()
{
	CloseSockets();
	FCoreUObjectDelegates::PostLoadMapWithWorld.RemoveAll(this);
	if (GEngine) GEngine->OnNetworkFailure().RemoveAll(this);
	Super::Shutdown();
}

// The world on screen lives on the client alone; the server has no picture to draw.
void UCubeWorldGameInstance::HandlePostLoadMap(UWorld* LoadedWorld)
{
	if (!LoadedWorld || CubeIsServerProcess() || LoadedWorld->GetNetMode() == NM_DedicatedServer) return;
	// The world is drawn the moment the map is up, not when the server's game state arrives (a network client's BeginPlay
	// waits for it): after a crossing the screen shows the same world at once.
	if (ACubeWorldActor* WorldActor = LoadedWorld->SpawnActor<ACubeWorldActor>()) WorldActor->Init();
	// The crossing pose is the latest one the old world had, not the one from when the travel began: the player walked on.
	if (Crossing.bSet && LastBody.bSet) { Crossing = LastBody; Crossing.bSet = true; }
	// Until the next server hands over the pawn the view stays at the player's eyes, not at the map's origin.
	if (Crossing.bSet && LoadedWorld->GetNetMode() == NM_Client)
	{
		EndCrossingView();
		const FVector Eye(Crossing.X * CubeSpec::BlockCm, Crossing.Y * CubeSpec::BlockCm, (Crossing.Z + CubeSpec::EyeHeight) * CubeSpec::BlockCm);
		const FRotator Look(-FMath::RadiansToDegrees(Crossing.Pitch), FMath::RadiansToDegrees(Crossing.Yaw) + 90.f, 0);
		CrossingEye = Eye; CrossingLook = Look;
		// The body goes on from the old pawn's very state, and the time the map took to load is walked too.
		GapBody = LastFullBody;
		GapAccumulator = LastAccumulator;
		GapLastTime = LastBodyTime > 0 ? LastBodyTime : FPlatformTime::Seconds();
		GapYawDeg = Look.Yaw; GapPitchDeg = Look.Pitch;
		GapFov = LastFov > 0 ? LastFov : CubeSpec::Fov;
		FString WalkTo;
		bGapTestWalk = FParse::Value(FCommandLine::Get(), TEXT("-walkto="), WalkTo, false) && !WalkTo.IsEmpty() && !FParse::Param(FCommandLine::Get(), TEXT("holdkeys"));
		bGapActive = true;
		CubeKeys::StartMouse();
		StepGap();
		CrossingCamera = LoadedWorld->SpawnActor<ACameraActor>(Eye, Look);
		// The same picture as the pawn's: no 16:9 bars (a camera actor constrains its aspect by default), the same field of view.
		if (CrossingCamera.IsValid())
		{
			CrossingCamera->GetCameraComponent()->SetConstraintAspectRatio(false);
			if (LastHorizontalFov > 0) CrossingCamera->GetCameraComponent()->SetFieldOfView(LastHorizontalFov);
		}
		// The engine draws nothing while the local player has no player controller, and the next server's arrives a round
		// trip after the map: a local placeholder (which the engine destroys when the real one comes, NetConnection.cpp)
		// shows the world from the crossing camera meanwhile.
		// The engine spawns a placeholder of its own in LoadMap; only when it has not is one made here.
		if (ULocalPlayer* LocalPlayer = GetFirstGamePlayer())
			if (!LocalPlayer->PlayerController)
			{
				FActorSpawnParameters Params;
				Params.ObjectFlags |= RF_Transient;
				if (APlayerController* Placeholder = LoadedWorld->SpawnActor<APlayerController>(APlayerController::StaticClass(), Eye, Look, Params))
				{
					Placeholder->SetPlayer(LocalPlayer);
					Placeholder->SetControlRotation(Look);
					AimPlaceholder(Placeholder);
					Placeholder->ClientSetHUD_Implementation(ACubeHUD::StaticClass());
				}
			}
		if (APlayerController* Existing = GetFirstLocalPlayerController()) if (Existing->GetLocalRole() == ROLE_Authority) { AimPlaceholder(Existing); Existing->ClientSetHUD_Implementation(ACubeHUD::StaticClass()); }
		CrossingBlankFrames = 0; CrossingWrongFrames = 0;
		// Held on every frame after the engine's own camera update (LevelTick: cameras, then the post-actor-tick hook), so a
		// view target the engine switches to on its own (the next server's controller, its pawn still where that server
		// spawned it) is never drawn.
		CrossingViewUntil = FPlatformTime::Seconds() + 5;
		if (FParse::Param(FCommandLine::Get(), TEXT("logcrossing")) && GEngine && GEngine->GameViewport && !DrawLogHandle.IsValid())
		{
			DrawLogUntil = FMath::Max(DrawLogUntil, FPlatformTime::Seconds() + 3);
			DrawLogHandle = GEngine->GameViewport->OnBeginDraw().AddUObject(this, &UCubeWorldGameInstance::LogDrawnFrame);
		}
		CrossingViewTicker = FWorldDelegates::OnWorldPostActorTick.AddUObject(this, &UCubeWorldGameInstance::HoldCrossingView);
		// The world does not tick until the server's game state says play has begun, but frames are drawn all along: the
		// view is aimed right before each draw as well.
		if (GEngine && GEngine->GameViewport) CrossingDrawHandle = GEngine->GameViewport->OnBeginDraw().AddUObject(this, &UCubeWorldGameInstance::AimCrossingViewBeforeDraw);
	}
	// Back in the local map after an Unreal server: the C# server waiting for us is reached now.
	if (SocketPlan.bSet && LoadedWorld->GetNetMode() == NM_Standalone)
	{
		const FSocketPlan Plan = SocketPlan;
		SocketPlan = FSocketPlan();
		ConnectSocket(Plan.RoomName, Plan.Host, Plan.Port, Plan.bSecure, Plan.ReservationToken, Plan.bTeleport);
	}
}

void UCubeWorldGameInstance::StepGap()
{
	if (!bGapActive) return;
	const double Now = FPlatformTime::Seconds();
	const double Dt = FMath::Clamp(Now - GapLastTime, 0.0, 0.25);
	GapLastTime = Now;
	// The mouse turns the view as the pawn's OnTurn and OnLookUp do (MouseX and MouseY at 0.15 degrees each).
	const FVector2D Mouse = CubeKeys::TakeMouse();
	GapYawDeg += Mouse.X * CubeSpec::DegreesPerMousePixel;
	GapPitchDeg = FMath::Clamp(GapPitchDeg + (float)Mouse.Y * CubeSpec::DegreesPerMousePixel, -89.9f, 89.9f);
	FCubeInput Input;
	FCubeKeys Keys;
	if (CubeKeys::Read(Keys))
	{
		Input.Forward = Keys.Forward; Input.Strafe = -Keys.Strafe;   // Minecraft's strafe is positive to the left
		Input.bJump = Keys.bJump; Input.bSprint = Keys.bSprint; Input.bSneak = Keys.bSneak;
	}
	if (bGapTestWalk) { Input.Forward = 1; Input.bSprint = true; }
	Input.Yaw = CubeSpec::YawFromUnreal(GapYawDeg);
	const FCubeSolidQuery Solid = [this](int32 X, int32 Y, int32 Z) { return World.IsSolidForPhysics(X, Y, Z); };
	GapAccumulator += Dt;
	while (GapAccumulator >= CubeSpec::TickSeconds) { CubePhysics::Tick(GapBody, Input, Solid); GapAccumulator -= CubeSpec::TickSeconds; }
	const double Partial = GapAccumulator / CubeSpec::TickSeconds;
	const FVector Feet(GapBody.PX + (GapBody.X - GapBody.PX) * Partial, GapBody.PY + (GapBody.Y - GapBody.PY) * Partial, GapBody.PZ + (GapBody.Z - GapBody.PZ) * Partial);
	CrossingEye = FVector(Feet.X, Feet.Y, Feet.Z + GapBody.EyeHeight()) * CubeSpec::BlockCm;
	CrossingLook = FRotator(GapPitchDeg, GapYawDeg, 0);
	// The field of view follows the sprint exactly as the pawn's does.
	const float TargetFov = CubeSpec::Fov * (GapBody.bSprinting ? CubeSpec::SprintFov : 1.f);
	GapFov += (TargetFov - GapFov) * FMath::Min(1.f, (float)Dt * 12.f);
	float Aspect = 16.f / 9.f;
	if (GEngine && GEngine->GameViewport) { FVector2D Size; GEngine->GameViewport->GetViewportSize(Size); if (Size.Y > 0) Aspect = Size.X / Size.Y; }
	LastFov = GapFov;
	LastHorizontalFov = FMath::RadiansToDegrees(2.f * FMath::Atan(FMath::Tan(FMath::DegreesToRadians(GapFov / 2.f)) * Aspect));
	// The next server hears of the player where they are now.
	Crossing.X = GapBody.X; Crossing.Y = GapBody.Y; Crossing.Z = GapBody.Z;
	Crossing.VX = GapBody.VX; Crossing.VY = GapBody.VY; Crossing.VZ = GapBody.VZ;
	Crossing.Yaw = CubeSpec::YawFromUnreal(GapYawDeg); Crossing.Pitch = CubeSpec::PitchFromUnreal(GapPitchDeg);
	Crossing.bSprinting = GapBody.bSprinting; Crossing.bSneaking = GapBody.bSneaking;
	Crossing.Forward = Input.Forward; Crossing.Strafe = -Input.Strafe;
}

void UCubeWorldGameInstance::AimPlaceholder(APlayerController* PC)
{
	if (!PC) return;
	PC->SetInitialLocationAndRotation(CrossingEye, CrossingLook);
	PC->SetControlRotation(CrossingLook);
	if (PC->GetViewTarget() != PC) PC->SetViewTarget(PC);
	if (APlayerCameraManager* Cam = PC->PlayerCameraManager)
	{
		if (LastHorizontalFov > 0) { Cam->DefaultFOV = LastHorizontalFov; Cam->SetFOV(LastHorizontalFov); }
		Cam->UpdateCamera(0.f);
	}
}

void UCubeWorldGameInstance::AimCrossingViewBeforeDraw()
{
	StepGap();
	APlayerController* PC = GetFirstLocalPlayerController();
	if (!PC || PC->GetLocalRole() != ROLE_Authority) return;
	FVector Loc; FRotator Rot; PC->GetPlayerViewPoint(Loc, Rot);
	if (FVector::Dist(Loc, CrossingEye) > 300) CrossingWrongFrames++;
	AimPlaceholder(PC);
	if (DrawLogHandle.IsValid())
	{
		FVector After; FRotator AfterRot; PC->GetPlayerViewPoint(After, AfterRot);
		APlayerCameraManager* Cam = PC->PlayerCameraManager;
		UE_LOG(LogCubeWorld, Log, TEXT("  aimed: before %s, after %s, actor %s, cam %s, cache t=%.3f, world t=%.3f, pc %p"), *Loc.ToString(), *After.ToString(), *(PC->GetRootComponent() ? PC->GetRootComponent()->GetComponentLocation() : FVector::ZeroVector).ToString(), Cam ? *Cam->GetCameraLocation().ToString() : TEXT("none"), Cam ? Cam->GetCameraCacheTime() : -1.f, GetWorld() ? GetWorld()->GetTimeSeconds() : -1.f, PC);
	}
}

void UCubeWorldGameInstance::HoldCrossingView(UWorld* InWorld, ELevelTick, float)
{
	if (!InWorld || InWorld->GetNetMode() == NM_DedicatedServer) return;
	if (!CrossingCamera.IsValid() || FPlatformTime::Seconds() > CrossingViewUntil) { EndCrossingView(); return; }
	StepGap();
	APlayerController* PC = GetFirstLocalPlayerController();
	if (!PC) { CrossingBlankFrames++; return; }
	// The placeholder is the view itself, at the player's eyes: a fresh world's camera cache is not trusted by the engine in
	// its first frames (its timestamp is 0), so the view falls back to the controller's own place, which must be right.
	if (PC->GetLocalRole() == ROLE_Authority)
	{
		FVector Loc; FRotator Rot; PC->GetPlayerViewPoint(Loc, Rot);
		if (FVector::Dist(Loc, CrossingEye) > 300) CrossingWrongFrames++;
		AimPlaceholder(PC);
	}
	// The HUD stays on screen: the next server's controller gets it at once, not when the server's call arrives.
	if (!PC->GetHUD() || !PC->GetHUD()->IsA(ACubeHUD::StaticClass())) PC->ClientSetHUD_Implementation(ACubeHUD::StaticClass());
}

void UCubeWorldGameInstance::LogDrawnFrame()
{
	LastDrawnFrame = GFrameCounter;
	if (FPlatformTime::Seconds() > DrawLogUntil) { if (GEngine && GEngine->GameViewport) GEngine->GameViewport->OnBeginDraw().Remove(DrawLogHandle); DrawLogHandle.Reset(); return; }
	APlayerController* PC = GetFirstLocalPlayerController();
	FString Line = FString::Printf(TEXT("frame %llu: "), (unsigned long long)GFrameCounter);
	if (!PC) Line += TEXT("no player controller");
	else
	{
		FVector Loc; FRotator Rot;
		PC->GetPlayerViewPoint(Loc, Rot);
		const AActor* Target = PC->GetViewTarget();
		Line += FString::Printf(TEXT("view %.3f %.3f %.3f yaw %.1f fov %.1f target %s pc %s pawn %s"), Loc.X / CubeSpec::BlockCm, Loc.Y / CubeSpec::BlockCm, Loc.Z / CubeSpec::BlockCm, Rot.Yaw, PC->PlayerCameraManager ? PC->PlayerCameraManager->GetFOVAngle() : 0.f, Target ? *Target->GetClass()->GetName() : TEXT("none"), PC->GetLocalRole() == ROLE_Authority ? TEXT("placeholder") : TEXT("server"), PC->GetPawn() ? *PC->GetPawn()->GetActorLocation().ToString() : TEXT("none"));
	}
	UE_LOG(LogCubeWorld, Log, TEXT("%s"), *Line);
}

void UCubeWorldGameInstance::LogEndOfFrame()
{
	if (FPlatformTime::Seconds() > DrawLogUntil) { FCoreDelegates::OnEndFrame.Remove(EndFrameLogHandle); EndFrameLogHandle.Reset(); return; }
	FVector2D Size = FVector2D::ZeroVector;
	if (GEngine && GEngine->GameViewport) GEngine->GameViewport->GetViewportSize(Size);
	UE_LOG(LogCubeWorld, Log, TEXT("end of frame %llu: drawn %d, focus %d, viewport %.0fx%.0f, world %s"), (unsigned long long)GFrameCounter, LastDrawnFrame == GFrameCounter ? 1 : 0, FApp::HasFocus() ? 1 : 0, Size.X, Size.Y, GEngine && GEngine->GameViewport && GEngine->GameViewport->GetWorld() ? *GEngine->GameViewport->GetWorld()->GetName() : TEXT("none"));
}

void UCubeWorldGameInstance::EndCrossingView()
{
	if (bGapActive) { bGapActive = false; CubeKeys::StopMouse(); }
	if (CrossingCamera.IsValid()) Log(FString::Printf(TEXT("crossing: %d frame(s) without a view, %d held from a wrong place"), CrossingBlankFrames, CrossingWrongFrames));
	if (CrossingViewTicker.IsValid()) { FWorldDelegates::OnWorldPostActorTick.Remove(CrossingViewTicker); CrossingViewTicker.Reset(); }
	if (CrossingDrawHandle.IsValid()) { if (GEngine && GEngine->GameViewport) GEngine->GameViewport->OnBeginDraw().Remove(CrossingDrawHandle); CrossingDrawHandle.Reset(); }
	if (CrossingCamera.IsValid())
	{
		if (APlayerController* PC = GetFirstLocalPlayerController()) if (PC->GetPawn()) PC->SetViewTarget(PC->GetPawn());
		CrossingCamera->Destroy();
	}
	CrossingCamera.Reset();
}

void UCubeWorldGameInstance::Log(const FString& Text)
{
	UE_LOG(LogCubeWorld, Log, TEXT("%s"), *Text);
	LogLines.Insert(Text, 0);
	if (LogLines.Num() > 8) LogLines.SetNum(8);
}

void UCubeWorldGameInstance::StartPlay(const FString& Name)
{
	if (bSigningIn || IsConnected() || bSwitching) return;
	PlayerName = Name;
	bSigningIn = true;
	Status = TEXT("Signing in...");
	if (PlayServ::Auth::IsLoggedIn() && !PlayerId.IsEmpty()) { Browse(); return; }
	FString Kept;
	if (!FFileHelper::LoadFileToString(Kept, *GuestFile(Name)) || Kept.TrimStartAndEnd().IsEmpty()) { SignInAsNewGuest(); return; }
	TWeakObjectPtr<UCubeWorldGameInstance> Weak(this);
	PlayServ::Auth::LoginWithRefreshToken(Kept.TrimStartAndEnd(), FPlayServAuthCallback::CreateLambda([Weak](bool bOk, const FString& InPlayerId, const FPlayServError& Error)
	{
		if (!Weak.IsValid()) return;
		UCubeWorldGameInstance* Self = Weak.Get();
		if (bOk)
		{
			Self->PlayerId = InPlayerId;
			Self->Log(FString::Printf(TEXT("signed in again as %s"), *Self->PlayerName));
			Self->Browse();
			return;
		}
		// Offline, the platform never saw the token: it is kept for the next try.
		if (Error.Code == EPlayServErrorCode::NetworkUnreachable || Error.Code == EPlayServErrorCode::Timeout)
		{
			Self->bSigningIn = false;
			Self->Status = FString::Printf(TEXT("Sign-in failed: %s"), *Error.Message);
			Self->Log(Self->Status);
			return;
		}
		// Refused, the token is spent: a new guest under the same name.
		IFileManager::Get().Delete(*GuestFile(Self->PlayerName), false, true, true);
		Self->SignInAsNewGuest();
	}));
}

void UCubeWorldGameInstance::SignInAsNewGuest()
{
	TWeakObjectPtr<UCubeWorldGameInstance> Weak(this);
	PlayServ::Auth::LoginAnonymous(PlayerName, FPlayServAuthCallback::CreateLambda([Weak](bool bOk, const FString& InPlayerId, const FPlayServError& Error)
	{
		if (!Weak.IsValid()) return;
		UCubeWorldGameInstance* Self = Weak.Get();
		if (!bOk)
		{
			Self->bSigningIn = false;
			Self->Status = FString::Printf(TEXT("Sign-in failed: %s"), *Error.Message);
			Self->Log(Self->Status);
			return;
		}
		Self->PlayerId = InPlayerId;
		Self->Log(FString::Printf(TEXT("signed in as %s"), *Self->PlayerName));
		Self->Browse();
	}));
}

// An operator's close is followed by the room opening again fresh within a minute or two; an operator's removal holds
// for as long as that room lives. The same rules as web/rooms.js.
FString UCubeWorldGameInstance::TurnedAway(const FString& RoomName, const FString& ReasonOrCode)
{
	double Wait = 0;
	FString Message;
	if (ReasonOrCode.Contains(TEXT("room_closed")))
	{
		Wait = 30;
		Message = TEXT("This room was closed by an operator. It opens again fresh in a minute or two.");
	}
	else if (ReasonOrCode.Contains(TEXT("removed")))
	{
		Wait = 60;
		Message = TEXT("An operator removed you from this room. You can still walk into the other regions.");
	}
	if (!Message.IsEmpty() && !RoomName.IsEmpty()) NotBefore.Add(RoomName, FPlatformTime::Seconds() + Wait);
	return Message;
}

void UCubeWorldGameInstance::TurnedAwayBy(const FString& Reason)
{
	const FString Message = TurnedAway(Room.IsEmpty() ? Travelling : Room, Reason);
	Log(Message.IsEmpty() ? FString::Printf(TEXT("turned away: %s"), *Reason) : Message);
}

void UCubeWorldGameInstance::Browse()
{
	if (BrowsesPending > 0) return;
	Status = TEXT("Looking for servers...");
	// The C# servers and the Unreal servers register under their own room types; both are listed, and a room is
	// joined under the type it was found in. A type the project has not got simply lists nothing.
	BrowseFound.Empty();
	BrowseError.Empty();
	BrowsesPending = CubeSpec::RoomTypes().Num();
	TWeakObjectPtr<UCubeWorldGameInstance> Weak(this);
	for (const FString& Slug : CubeSpec::RoomTypes())
	{
		PlayServ::Rooms::Browse(Slug, FPlayServRoomFilters(), FPlayServBrowseCallback::CreateLambda([Weak, Slug](bool bOk, const FPlayServBrowsePage& Page, const FPlayServError& Error)
		{
			if (!Weak.IsValid()) return;
			UCubeWorldGameInstance* Self = Weak.Get();
			if (bOk)
				for (const FPlayServRoomListing& R : Page.Rooms)
				{
					Self->RoomSlugs.Add(R.RoomName, Slug);
					if ((R.PlacementState == EPlayServPlacementState::Open || R.PlacementState == EPlayServPlacementState::Unknown) && Self->MayTry(R.RoomName)) Self->BrowseFound.AddUnique(R.RoomName);
				}
			else if (Error.ProblemCode != TEXT("room_type_not_found") && Error.ProblemCode != TEXT("not_found")) Self->BrowseError = Error.Message;
			if (--Self->BrowsesPending > 0) return;
			Self->Browsed();
		}));
	}
}

void UCubeWorldGameInstance::Browsed()
{
	TWeakObjectPtr<UCubeWorldGameInstance> Weak(this);
	if (BrowseFound.Num() == 0)
	{
		if (!BrowseError.IsEmpty() || RoomSlugs.Num() == 0)
		{
			bSigningIn = false;
			Status = BrowseError.IsEmpty() ? TEXT("No server is running. Press Enter to retry.") : FString::Printf(TEXT("Browse failed: %s"), *BrowseError);
			Log(Status);
			return;
		}
		Status = TEXT("Every server is closing or full, retrying...");
		GetTimerManager().SetTimer(RetryTimer, [Weak]() { if (Weak.IsValid()) Weak->Browse(); }, 3.f, false);
		return;
	}
	Candidates = BrowseFound;
	// The Unreal servers first, the C# ones after: the Unreal client is at home on Iris.
	Candidates.Sort([this](const FString& A, const FString& B)
	{
		const bool bUnrealA = RoomSlugs.FindRef(A) == TEXT("cubeworld-ue"), bUnrealB = RoomSlugs.FindRef(B) == TEXT("cubeworld-ue");
		return bUnrealA != bUnrealB ? bUnrealA : A < B;
	});
	const FString First = Candidates[0];
	Candidates.RemoveAt(0);
	Enter(First, true);
}

void UCubeWorldGameInstance::Enter(const FString& RoomName, bool bTeleport)
{
	if (RoomName == Room || bSwitching) return;
	bSwitching = true;
	Status = FString::Printf(TEXT("Joining %s..."), *RoomName);
	TWeakObjectPtr<UCubeWorldGameInstance> Weak(this);
	// The SDK is not handed the controller: the travel is ours, so the position survives a border crossing.
	const FPlayServJoinCallback Joined = FPlayServJoinCallback::CreateLambda([Weak, RoomName, bTeleport](bool bOk, const FPlayServJoinResult& Result, const FPlayServError& Error)
	{
		if (!Weak.IsValid()) return;
		UCubeWorldGameInstance* Self = Weak.Get();
		if (!bOk || !Result.Ticket.Connect.IsSet())
		{
			// A room joined under the wrong room type is not found: a region's claim can carry a stale type (a C# server that
			// writes no type leaves the Unreal one that held the region before). The other type is tried once, at once.
			if (!bOk && (Error.ProblemCode == TEXT("room_not_found") || Error.ProblemCode == TEXT("room_type_not_found") || Error.ProblemCode == TEXT("not_found")) && !Self->RetriedOtherType.Contains(RoomName))
			{
				const FString Tried = Self->RoomSlugs.FindRef(RoomName);
				const FString Other = Tried == TEXT("cubeworld-ue") ? FString(TEXT("cubeworld")) : FString(TEXT("cubeworld-ue"));
				Self->RetriedOtherType.Add(RoomName);
				Self->RoomSlugs.Add(RoomName, Other);
				Self->Log(FString::Printf(TEXT("%s is not a %s room, trying %s"), *RoomName, Tried.IsEmpty() ? TEXT("default") : *Tried, *Other));
				Self->bSwitching = false;
				Self->Enter(RoomName, bTeleport);
				return;
			}
			Self->bSwitching = false;
			Self->CrossAfter = FPlatformTime::Seconds() + 3;
			const FString Turned = Self->TurnedAway(RoomName, Error.ProblemCode);
			Self->Log(Turned.IsEmpty() ? FString::Printf(TEXT("%s refused the join: %s"), *RoomName, *Error.Message) : Turned);
			if (!Self->IsConnected())
			{
				if (Self->Candidates.Num() > 0) { const FString NextRoom = Self->Candidates[0]; Self->Candidates.RemoveAt(0); Self->Enter(NextRoom, bTeleport); }
				else Self->GetTimerManager().SetTimer(Self->RetryTimer, [Weak]() { if (Weak.IsValid()) Weak->Browse(); }, 3.f, false);
			}
			return;
		}
		const FPlayServRoomConnect& C = Result.Ticket.Connect;
		// An Unreal server says so in its attributes and is reached over Iris; any other room is a C# server, reached
		// over the JSON socket, plain or TLS (the SDK reads wss as anything but ws).
		const bool bUnreal = Result.Ticket.Attributes.FindRef(TEXT("engine")) == TEXT("unreal");
		// -forcews enters an Unreal server through its JSON door instead of Iris, for testing the door.
		const bool bDoorOnly = FParse::Param(FCommandLine::Get(), TEXT("forcews"));
		if (bUnreal && !bDoorOnly)
		{
			// A pool room's connect is the platform's wss front; Iris's UDP address rides in the attribute udp.
			// By the machine's address, not its name: a name lookup costs half a second at every crossing.
			FString Udp = Result.Ticket.Attributes.FindRef(TEXT("udp"));
			const FString Ip = Result.Ticket.Attributes.FindRef(TEXT("playserv_public_ip"));
			FString UdpHost, UdpPort;
			if (!Ip.IsEmpty() && Udp.Split(TEXT(":"), &UdpHost, &UdpPort, ESearchCase::IgnoreCase, ESearchDir::FromEnd)) Udp = Ip + TEXT(":") + UdpPort;
			const FString Url = Udp.IsEmpty() ? PlayServ::Rooms::BuildTravelUrl(Result.Ticket) : FString::Printf(TEXT("%s?rsv=%s"), *Udp, *Result.Ticket.ReservationToken);
			Self->TravelToUnrealServer(RoomName, Url, bTeleport);
			return;
		}
		FString Host = C.Host, Override;
		int32 Port = C.Port;
		bool bSecure = C.Transport != EPlayServRoomTransport::Ws;
		const FString Door = Result.Ticket.Attributes.FindRef(TEXT("ws"));
		if (bUnreal && !Door.IsEmpty())
		{
			// ws://host:port or wss://host:port
			FString Scheme, Rest, PortText;
			if (Door.Split(TEXT("://"), &Scheme, &Rest)) { bSecure = Scheme == TEXT("wss"); if (Rest.Split(TEXT(":"), &Host, &PortText)) Port = FCString::Atoi(*PortText); else Host = Rest; }
		}
		if (FParse::Param(FCommandLine::Get(), TEXT("wsplain"))) bSecure = false;
		if (FParse::Param(FCommandLine::Get(), TEXT("wssecure"))) bSecure = true;
		if (FParse::Value(FCommandLine::Get(), TEXT("-wshost="), Override) && !Override.IsEmpty())
		{
			FString PortText;
			if (Override.Split(TEXT(":"), &Host, &PortText)) Port = FCString::Atoi(*PortText); else Host = Override;
		}
		Self->ConnectSocket(RoomName, Host, Port, bSecure, Result.Ticket.ReservationToken, bTeleport);
	});
	// The room is joined under the type it was listed in; one heard of only by name goes under the default type.
	const FString Slug = RoomSlugs.FindRef(RoomName);
	if (Slug.IsEmpty()) PlayServ::Rooms::JoinRoom(RoomName, nullptr, Joined);
	else PlayServ::Rooms::JoinRoom(Slug, RoomName, nullptr, Joined);
}

void UCubeWorldGameInstance::TravelToUnrealServer(const FString& RoomName, const FString& Url, bool bTeleport)
{
	APlayerController* PC = GetFirstLocalPlayerController();
	if (!PC) { bSwitching = false; return; }
	CloseSockets();
	bViaSocket = false;
	// A crossing keeps the body where it is; a fresh join takes the server's spawn.
	Crossing = bTeleport ? FCubeCrossing() : LastBody;
	Crossing.bSet = !bTeleport && bPlaced;
	bWelcomed = false;
	bWorldLoaded = false;
	Travelling = RoomName;
	// A crossing keeps the players it knows: the next world shows them at once, where they were.
	if (!Crossing.bSet) Players.Empty();
	if (FParse::Param(FCommandLine::Get(), TEXT("logcrossing")) && GEngine && GEngine->GameViewport)
	{
		DrawLogUntil = FPlatformTime::Seconds() + 5;
		if (!DrawLogHandle.IsValid()) DrawLogHandle = GEngine->GameViewport->OnBeginDraw().AddUObject(this, &UCubeWorldGameInstance::LogDrawnFrame);
		if (!EndFrameLogHandle.IsValid()) EndFrameLogHandle = FCoreDelegates::OnEndFrame.AddUObject(this, &UCubeWorldGameInstance::LogEndOfFrame);
	}
	Log(FString::Printf(TEXT("travelling to %s (Unreal) at %s"), *RoomName, *Url.Left(Url.Find(TEXT("?")) > 0 ? Url.Find(TEXT("?")) : Url.Len())));
	PC->ClientTravel(Url, ETravelType::TRAVEL_Absolute);
}

void UCubeWorldGameInstance::HandleNetworkFailure(UWorld* InWorld, UNetDriver* NetDriver, ENetworkFailure::Type FailureType, const FString& ErrorString)
{
	if (CubeIsServerProcess()) return;
	if (SocketPlan.bSet) return;   // leaving an Unreal server on purpose, for a C# one
	Disconnected(ErrorString.IsEmpty() ? FString(ENetworkFailure::ToString(FailureType)) : ErrorString);
}

// A kick (an operator's close or removal) brings the client back to the menu map; look for a server again after it.
void UCubeWorldGameInstance::ReturnToMainMenu()
{
	Super::ReturnToMainMenu();
	if (!CubeIsServerProcess()) Disconnected(TEXT("the server closed the connection"));
}

void UCubeWorldGameInstance::Disconnected(const FString& Why)
{
	if (!bWelcomed && !bSwitching && Travelling.IsEmpty()) return;
	Crossing = FCubeCrossing();
	if (Socket.IsValid()) { Socket->Close(); Socket.Reset(); }
	bViaSocket = false;
	const FString Turned = TurnedAway(Room.IsEmpty() ? Travelling : Room, Why);
	Log(Turned.IsEmpty() ? FString::Printf(TEXT("disconnected: %s"), *Why) : Turned);
	bWelcomed = false;
	bSwitching = false;
	bSigningIn = false;
	Room.Empty();
	Travelling.Empty();
	CrossAfter = FPlatformTime::Seconds() + 3;
	Reconnect();
}

// After the connection goes: look for a server again in a moment, keeping the sign-in.
void UCubeWorldGameInstance::Reconnect()
{
	Status = TEXT("Disconnected, reconnecting...");
	TWeakObjectPtr<UCubeWorldGameInstance> Weak(this);
	GetTimerManager().SetTimer(RetryTimer, [Weak]() { if (Weak.IsValid() && !Weak->IsConnected() && !Weak->bSwitching && !Weak->PlayerId.IsEmpty()) { Weak->bSigningIn = true; Weak->Browse(); } }, 3.f, false);
}

FString UCubeWorldGameInstance::RoomOfRegion(int32 InRegion) const
{
	for (const FCubeRegion& R : Regions) if (R.Region == InRegion) return R.Room;
	return FString();
}

void UCubeWorldGameInstance::MaybeCross(double X, double Y)
{
	if (!bPlaced || bSwitching || !IsConnected() || FPlatformTime::Seconds() < CrossAfter) return;
	const FString Here = RoomOfRegion(CubeSpec::RegionOf(X, Y));
	if (!Here.IsEmpty() && Here != Room && MayTry(Here)) Enter(Here, false);
}

FString UCubeWorldGameInstance::NameOf(const FString& Id) const
{
	if (Id == PlayerId) return TEXT("you");
	for (const FCubePresence& P : Players) if (P.Id == Id) return P.Name;
	return Id;
}

// ── what the server tells this client ────────────────────────────────────────────────────────────

void UCubeWorldGameInstance::OnWelcomed(const FString& InServer, const FString& InColor, const FString& InRoom, int32 InRegion, const FCubePose& You, const TArray<FCubeStackRep>& Stacks, int32 ChunkCount)
{
	const bool bCrossed = Crossing.bSet;
	if (bCrossed) bCrossedOnce = true;
	RetriedOtherType.Remove(InRoom);
	Server = InServer; Color = InColor; Room = InRoom; Region = InRegion;
	Travelling.Empty();
	bSwitching = false;
	bSigningIn = false;
	bWelcomed = true;
	Health = You.Health;
	bDead = false;
	Status.Empty();
	// A fresh join starts from the generated terrain; a crossing keeps the world on screen and applies only what differs.
	bSnapshotDiff = bCrossed;
	Snapshot.Reset();
	if (!bCrossed) World.Clear();
	bWorldLoaded = false;
	ChunksExpected = ChunkCount;
	ChunksReceived = 0;
	PendingCubes.Empty();
	SetInventory(Stacks);
	// The server placed the player where the hello asked: at the crossing point, or at its spawn.
	WelcomePose = You;
	Crossing = FCubeCrossing();
	Log(FString::Printf(TEXT("%s %s"), bCrossed ? TEXT("crossed into") : TEXT("entered"), *InRoom));
	if (ChunkCount == 0) OnWorldChunk(TArray<FCubeCellRep>(), true);
}

void UCubeWorldGameInstance::ApplySnapshot()
{
	TMap<FIntVector, FName> Next;
	for (const auto& P : Snapshot) if (P.Value != NAME_None) Next.Add(P.Key, P.Value);
	const TArray<FIntVector> Changed = World.ReplaceOverrides(Next);
	Snapshot.Reset();
	bSnapshotDiff = false;
	if (Changed.Num() > 0) OnCubes.Broadcast(Changed);
}

// The body is placed only once the whole world is here: it must not fall through blocks that have not arrived.
void UCubeWorldGameInstance::OnWorldChunk(const TArray<FCubeCellRep>& Cells, bool bLast)
{
	for (const FCubeCellRep& C : Cells)
	{
		if (bSnapshotDiff) Snapshot.Add(FIntVector(C.X, C.Y, C.Z), CubeSpec::KindOf(C.Kind));
		else World.Set(C.X, C.Y, C.Z, CubeSpec::KindOf(C.Kind));
	}
	ChunksReceived++;
	if (!bLast) return;
	bWorldLoaded = true;
	if (bSnapshotDiff) ApplySnapshot();
	OnWelcome.Broadcast(WelcomePose, !bSnapshotDiff);
	bPlaced = true;
	for (const FPendingCubes& P : PendingCubes) ApplyCubes(P.Changes, P.Falls, P.bRemote);
	PendingCubes.Empty();
}

void UCubeWorldGameInstance::ApplyCubes(const TArray<FCubeChangeRep>& Changes, const TArray<FCubeFallRep>& Falls, bool bRemote)
{
	if (!bWorldLoaded) { PendingCubes.Add({ Changes, Falls, bRemote }); return; }
	for (const FCubeFallRep& F : Falls) OnFall.Broadcast(CubeSpec::KindOf(F.Kind), F.X, F.Y, F.FromZ, F.ToZ);
	TArray<FIntVector> Changed;
	FString On;
	for (const FCubeChangeRep& C : Changes)
	{
		const FName Kind = CubeSpec::KindOf(C.Kind);
		World.Set(C.X, C.Y, C.Z, Kind);
		Changed.Add(FIntVector(C.X, C.Y, C.Z));
		OnCube.Broadcast(C.X, C.Y, C.Z, Kind);
		if (On.IsEmpty()) On = C.On;
	}
	OnCubes.Broadcast(Changed);
	if (Changed.Num() > 0 && bRemote)
		Log(FString::Printf(TEXT("%d block%s changed on server %s -> arrived here"), Changed.Num(), Changed.Num() > 1 ? TEXT("s") : TEXT(""), *On));
}

void UCubeWorldGameInstance::SetInventory(const TArray<FCubeStackRep>& Stacks)
{
	Inventory.Empty();
	for (const FCubeStackRep& S : Stacks) Inventory.Add(CubeSpec::KindOf(S.Kind), S.Count);
	OnInventory.Broadcast();
}

void UCubeWorldGameInstance::SetPlayers(const TArray<FCubePresenceRep>& InPlayers)
{
	Players.Empty();
	for (const FCubePresenceRep& R : InPlayers)
	{
		FCubePresence P;
		P.Id = R.Id; P.Name = R.Name; P.Server = R.Server; P.Color = R.Color;
		P.X = R.X; P.Y = R.Y; P.Z = R.Z; P.Yaw = R.Yaw; P.Pitch = R.Pitch; P.Health = R.Health;
		P.bSneaking = R.bSneaking; P.bSprinting = R.bSprinting;
		Players.Add(P);
	}
	OnPlayers.Broadcast(Players);
}

void UCubeWorldGameInstance::SetRegions(const TArray<FCubeRegionRep>& InRegions)
{
	Regions.Empty();
	for (const FCubeRegionRep& R : InRegions)
	{
		Regions.Add({ R.Region, R.Room, R.Color, R.Server, R.Slug });
		// The room type a browse found is the platform's own word; a region's claim only fills in rooms not browsed.
		if (!R.Slug.IsEmpty() && !RoomSlugs.Contains(R.Room)) RoomSlugs.Add(R.Room, R.Slug);
	}
}

void UCubeWorldGameInstance::OnHurtFrame(const FString& InPlayerId, double InHealth, double KX, double KY, double Strength)
{
	if (InPlayerId == PlayerId) Health = InHealth;
	OnHurt.Broadcast(InPlayerId, InHealth, KX, KY, Strength);
}

void UCubeWorldGameInstance::OnDeathFrame(const FString& InPlayerId, const FString& By)
{
	if (InPlayerId == PlayerId) { bDead = true; Health = 0; }
	OnDeath.Broadcast(InPlayerId, By);
}

void UCubeWorldGameInstance::OnRespawnFrame(const FCubePose& You)
{
	bDead = false;
	Health = You.Health;
	OnRespawn.Broadcast(You);
}

void UCubeWorldGameInstance::OnBombFrame(const FCubeBombRep& B)
{
	FCubeBombFrame F;
	F.Id = B.Id; F.State = B.State; F.Holder = B.Holder;
	F.X = B.X; F.Y = B.Y; F.Z = B.Z; F.VX = B.VX; F.VY = B.VY; F.VZ = B.VZ;
	F.Age = B.AgeMs;
	if (B.bHasHeight) F.Height = B.Height;
	OnBomb.Broadcast(F);
}
