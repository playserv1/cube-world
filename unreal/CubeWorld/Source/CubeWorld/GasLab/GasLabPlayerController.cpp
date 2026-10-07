#include "GasLab/GasLabPlayerController.h"
#include "GasLab/GasLabAbility.h"
#include "GasLab/GasLabAbilitySystemComponent.h"
#include "GasLab/GasLabAttributeSet.h"
#include "GasLab/GasLabCharacter.h"
#include "GasLab/GasLabClock.h"
#include "GasLab/GasLabPlayerState.h"
#include "GasLab/GasLabTags.h"
#include "CubeGameEngine.h"
#include "Components/InputComponent.h"
#include "Engine/Engine.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GasLabPlayerController)

namespace
{
	/** The autopilot outlives a server switch: each server gives the client a new controller. */
	struct FAutopilot
	{
		TArray<FVector> Waypoints;
		int32 Next = 0;
		double Joined = 0, WalkAt = 0, ThrowEvery = 0, LastThrow = 0;
		bool bRead = false;
		FVector Keys = FVector::ZeroVector;
	};

	FAutopilot& Pilot()
	{
		static FAutopilot P;
		if (!P.bRead)
		{
			P.bRead = true;
			FString Walk;
			if (FParse::Value(FCommandLine::Get(), TEXT("-gaswalk="), Walk, false))
			{
				TArray<FString> Points;
				Walk.ParseIntoArray(Points, TEXT(";"));
				for (const FString& Point : Points)
				{
					FString X, Y;
					if (Point.Split(TEXT(","), &X, &Y)) P.Waypoints.Add(FVector(FCString::Atof(*X), FCString::Atof(*Y), 0.f));
				}
			}
			FParse::Value(FCommandLine::Get(), TEXT("-gaswalkat="), P.WalkAt);
			FParse::Value(FCommandLine::Get(), TEXT("-gasthrowevery="), P.ThrowEvery);
		}
		return P;
	}
}

AGasLabPlayerController::AGasLabPlayerController()
{
	bShowMouseCursor = false;
}

void AGasLabPlayerController::ClientGasCross_Implementation(const FString& Address, const FString& Id, int32 ToRegion)
{
	UE_LOG(LogTemp, Log, TEXT("GASLAB client-cross id=%s to=%d address=%s at=%.3f"), *Id, ToRegion, *Address, GasLabClock::Now());
	if (!FParse::Param(FCommandLine::Get(), TEXT("gasnoseamless")))
	{
		if (UCubeGameEngine* Engine = UCubeGameEngine::Get()) Engine->RequestSeamlessTravel();
	}
	const FString Name = PlayerState ? PlayerState->GetPlayerName() : Id;
	ClientTravel(FString::Printf(TEXT("%s?gasplayer=%s?gasname=%s?gascross=1"), *Address, *Id, *Name), TRAVEL_Absolute);
}

void AGasLabPlayerController::ClientGasWalk_Implementation(FVector To)
{
	FAutopilot& P = Pilot();
	P.Waypoints.Add(To);
	P.WalkAt = 0;
	UE_LOG(LogTemp, Log, TEXT("GASLAB client-walk to=%.0f,%.0f at=%.3f"), To.X, To.Y, GasLabClock::Now());
}

void AGasLabPlayerController::ClientGasThrow_Implementation()
{
	Throw();
}

void AGasLabPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();
	if (!InputComponent) return;
	InputComponent->BindKey(EKeys::LeftMouseButton, IE_Pressed, this, &AGasLabPlayerController::Throw);
}

void AGasLabPlayerController::Throw()
{
	AGasLabCharacter* Me = Cast<AGasLabCharacter>(GetPawn());
	UGasLabAbilitySystemComponent* ASC = Me ? Me->GetLabASC() : nullptr;
	if (!ASC) return;
	const bool bOk = ASC->TryActivateAbilityByClass(UGasLabAbility_Throw::StaticClass());
	UE_LOG(LogTemp, Log, TEXT("GASLAB client-throw id=%s ok=%d cooldown=%.3f at=%.3f"), *Me->GetPlayerId(), bOk ? 1 : 0, ASC->TimeLeft(GasLabTags::Cooldown_Throw), GasLabClock::Now());
	Pilot().LastThrow = GasLabClock::Now();
}

void AGasLabPlayerController::PlayerTick(float DeltaTime)
{
	Super::PlayerTick(DeltaTime);
	if (!IsLocalController()) return;
	Autopilot(DeltaTime);
	// Keys, for a person at the window.
	if (APawn* Me = GetPawn())
	{
		const FRotator Yaw(0.f, GetControlRotation().Yaw, 0.f);
		FVector Move = FVector::ZeroVector;
		if (IsInputKeyDown(EKeys::W)) Move += Yaw.Vector();
		if (IsInputKeyDown(EKeys::S)) Move -= Yaw.Vector();
		if (IsInputKeyDown(EKeys::D)) Move += FRotationMatrix(Yaw).GetUnitAxis(EAxis::Y);
		if (IsInputKeyDown(EKeys::A)) Move -= FRotationMatrix(Yaw).GetUnitAxis(EAxis::Y);
		if (!Move.IsNearlyZero()) Me->AddMovementInput(Move.GetSafeNormal());
		float MX = 0.f, MY = 0.f;
		GetInputMouseDelta(MX, MY);
		AddYawInput(MX);
		AddPitchInput(-MY);
		if (IsInputKeyDown(EKeys::SpaceBar))
		{
			if (ACharacter* C = Cast<ACharacter>(Me)) C->Jump();
		}
	}
	const double Now = GasLabClock::Now();
	if (Now >= NextLog)
	{
		NextLog = Now + 0.1;
		LogState();
	}
	// -gasshotevery=<s>: a picture of the window that often, for the report (Saved/Screenshots/GasLab).
	static const float ShotEvery = []() { float S = 0.f; FParse::Value(FCommandLine::Get(), TEXT("-gasshotevery="), S); return S; }();
	static double NextShot = 0;
	static int32 Shots = 0;
	if (ShotEvery > 0.f && GetNetMode() == NM_Client && Now >= NextShot && GetPawn())
	{
		NextShot = Now + ShotEvery;
		const FString File = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Screenshots/GasLab"), FString::Printf(TEXT("shot_%05d_%.2f.png"), Shots++, Now));
		FScreenshotRequest::RequestScreenshot(File, true, false);
	}
}

void AGasLabPlayerController::Autopilot(float DeltaTime)
{
	FAutopilot& P = Pilot();
	AGasLabCharacter* Me = Cast<AGasLabCharacter>(GetPawn());
	if (!Me) return;
	const double Now = GasLabClock::Now();
	if (P.Joined == 0) P.Joined = Now;
	if (P.ThrowEvery > 0 && Now - P.LastThrow >= P.ThrowEvery && Now - P.Joined > 2.0) Throw();
	if (P.Next >= P.Waypoints.Num() || Now - P.Joined < P.WalkAt) return;
	const FVector To = P.Waypoints[P.Next];
	FVector D = To - Me->GetActorLocation();
	D.Z = 0.f;
	if (D.Size() < 40.f) { P.Next++; return; }
	SetControlRotation(FRotator(0.f, D.Rotation().Yaw, 0.f));
	Me->AddMovementInput(D.GetSafeNormal());
}

void AGasLabPlayerController::LogState()
{
	AGasLabCharacter* Me = Cast<AGasLabCharacter>(GetPawn());
	const AGasLabPlayerState* PS = GetPlayerState<AGasLabPlayerState>();
	UGasLabAbilitySystemComponent* ASC = Me ? Me->GetLabASC() : nullptr;
	if (!Me || !ASC || !PS) return;
	const float Hp = ASC->GetNumericAttribute(UGasLabAttributeSet::GetHealthAttribute());
	const float Burn = ASC->TimeLeft(GasLabTags::Effect_Burning);
	const int32 Stacks = ASC->Stacks(GasLabTags::Effect_Burning);
	const float Cooldown = ASC->TimeLeft(GasLabTags::Cooldown_Throw);
	const FVector P = Me->GetActorLocation();
	// How far the client's idea of the server's time is from the shared clock (the server's time, with -gasnoclock aside).
	const AGameStateBase* GS = GetWorld() ? GetWorld()->GetGameState() : nullptr;
	const double ClockError = GS ? (GS->GetServerWorldTimeSeconds() - GasLabClock::Now()) * 1000.0 : 0.0;
	UE_LOG(LogTemp, Log, TEXT("GASLAB client-state id=%s server=%d hp=%.2f burnleft=%.3f stacks=%d cooldown=%.3f clockerr=%.1fms pos=%.0f,%.0f at=%.3f"),
		*PS->LabId, PS->OwnerRegion, Hp, Burn, Stacks, Cooldown, ClockError, P.X, P.Y, GasLabClock::Now());
	if (GEngine && GetNetMode() == NM_Client)
	{
		GEngine->AddOnScreenDebugMessage(1, 0.5f, FColor::White, FString::Printf(TEXT("%s on server %d   HP %.0f   burning %.1f s x%d   throw ready in %.1f s"),
			*PS->LabId, PS->OwnerRegion, Hp, FMath::Max(0.f, Burn), Stacks, FMath::Max(0.f, Cooldown)));
	}
}

AGasLabBotController::AGasLabBotController()
{
	bWantsPlayerState = true;
	PrimaryActorTick.bCanEverTick = true;
}

void AGasLabBotController::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	APawn* Me = GetPawn();
	if (!Me || !WalkTo.IsSet()) return;
	FVector D = WalkTo.GetValue() - Me->GetActorLocation();
	D.Z = 0.f;
	if (D.Size() < 40.f) { WalkTo.Reset(); return; }
	SetControlRotation(FRotator(0.f, D.Rotation().Yaw, 0.f));
	Me->AddMovementInput(D.GetSafeNormal());
}
