#include "GasLab/GasLabGameState.h"
#include "GasLab/GasLabBorder.h"
#include "GasLab/GasLabClock.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "CubeShapes.h"
#include "Components/BoxComponent.h"
#include "EngineUtils.h"
#include "ProceduralMeshComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GasLabGameState)

double AGasLabGameState::GetServerWorldTimeSeconds() const
{
	if (GasLabClock::IsShared() && HasAuthority()) return GasLabClock::Now();
	return Super::GetServerWorldTimeSeconds();
}

void AGasLabGameState::UpdateServerTimeSeconds()
{
	Super::UpdateServerTimeSeconds();
	if (GasLabClock::IsShared()) ReplicatedWorldTimeSecondsDouble = GasLabClock::Now();
}

void AGasLabGameState::OnRep_ReplicatedWorldTimeSecondsDouble()
{
	UWorld* World = GetWorld();
	if (!World || !GasLabClock::IsShared() || FParse::Param(FCommandLine::Get(), TEXT("gasstocktime")))
	{
		Super::OnRep_ReplicatedWorldTimeSecondsDouble();
		return;
	}
	const double Sample = ReplicatedWorldTimeSecondsDouble - World->GetTimeSeconds();
	const bool bSnap = TimeSamples++ == 0 || FMath::Abs(Sample - ServerWorldTimeSecondsDelta) > 0.1;
	ServerWorldTimeSecondsDelta = bSnap ? float(Sample) : float(ServerWorldTimeSecondsDelta + (Sample - ServerWorldTimeSecondsDelta) * 0.2);
	if (bSnap && TimeSamples > 1)
	{
		// GAS derives each effect's local start from this estimate once; it must be told when the estimate moves.
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			APawn* Pawn = It->Get() ? It->Get()->GetPawn() : nullptr;
			if (UAbilitySystemComponent* ASC = UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(Pawn))
			{
				ASC->RecomputeGameplayEffectStartTimes(World->GetTimeSeconds(), GetServerWorldTimeSeconds());
			}
		}
	}
}

void AGasLabGameState::BeginPlay()
{
	Super::BeginPlay();
	AGasLabFloor::Ensure(GetWorld());
}

AGasLabFloor::AGasLabFloor()
{
	bReplicates = false;
	UBoxComponent* Box = CreateDefaultSubobject<UBoxComponent>(TEXT("Ground"));
	const float Half = UGasLabBorder::RegionSize * UGasLabBorder::Columns * 0.5f;
	Box->InitBoxExtent(FVector(Half + 2000.f, Half + 2000.f, 50.f));
	Box->SetCollisionProfileName(TEXT("BlockAll"));
	RootComponent = Box;
}

void AGasLabFloor::Ensure(UWorld* World)
{
	if (!World) return;
	for (TActorIterator<AGasLabFloor> It(World); It; ++It) return;
	const float Half = UGasLabBorder::RegionSize * UGasLabBorder::Columns * 0.5f;
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	World->SpawnActor<AGasLabFloor>(AGasLabFloor::StaticClass(), FVector(Half, Half, -50.f), FRotator::ZeroRotator, Params);
}

void AGasLabFloor::BeginPlay()
{
	Super::BeginPlay();
	if (GetNetMode() == NM_DedicatedServer) return;
	UProceduralMeshComponent* Mesh = NewObject<UProceduralMeshComponent>(this, TEXT("Tiles"));
	Mesh->SetupAttachment(RootComponent);
	Mesh->RegisterComponent();
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetMaterial(0, FCubeShape::Material());
	Mesh->SetAbsolute(true, true, true);
	FCubeShape Tiles;
	const float S = UGasLabBorder::RegionSize, Cell = 200.f;
	for (int32 R = 0; R < UGasLabBorder::Columns * UGasLabBorder::Rows; R++)
	{
		const FBox2D Box = UGasLabBorder::RegionBox(R);
		const FLinearColor Base = UGasLabBorder::RegionColor(R);
		for (float X = Box.Min.X; X < Box.Max.X; X += Cell)
			for (float Y = Box.Min.Y; Y < Box.Max.Y; Y += Cell)
			{
				const bool bOdd = (int32(X / Cell) + int32(Y / Cell)) % 2 == 1;
				const FLinearColor C = FLinearColor::LerpUsingHSV(Base, FLinearColor::White, bOdd ? 0.25f : 0.38f);
				Tiles.Box(FVector(X + Cell / 2, Y + Cell / 2, -2.f), FVector(Cell / 2, Cell / 2, 2.f), C);
			}
	}
	// The borders, in dark posts along both lines.
	for (float T = 0.f; T <= 2 * S; T += 100.f)
	{
		Tiles.Box(FVector(S, T, 4.f), FVector(6.f, 30.f, 4.f), FCubeShape::Hex(TEXT("#111827")));
		Tiles.Box(FVector(T, S, 4.f), FVector(30.f, 6.f, 4.f), FCubeShape::Hex(TEXT("#111827")));
	}
	Tiles.Commit(Mesh);
	// A pale sky all round, so the scene is not drawn on black.
	UProceduralMeshComponent* Sky = NewObject<UProceduralMeshComponent>(this, TEXT("Sky"));
	Sky->SetupAttachment(RootComponent);
	Sky->RegisterComponent();
	Sky->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Sky->SetMaterial(0, FCubeShape::Material());
	Sky->SetAbsolute(true, true, true);
	Sky->SetCastShadow(false);
	FCubeShape Dome;
	Dome.bShaded = false;
	Dome.Sphere(FVector(S, S, 0.f), 30000.f, 24, 12, [](int32) { return FCubeShape::Hex(TEXT("#bfdbfe")); }, PI, true);
	Dome.Commit(Sky);
}
