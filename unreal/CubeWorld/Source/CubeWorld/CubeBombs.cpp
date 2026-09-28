#include "CubeBombs.h"
#include "CubeShapes.h"
#include "CubeSpec.h"
#include "CubeVoxelWorld.h"
#include "ProceduralMeshComponent.h"
#include "Engine/World.h"

double CubeBombs::Descend(const FCubeVoxelWorld& World, double X, double Y, double Z)
{
	const int32 BX = FMath::FloorToInt32(X), BY = FMath::FloorToInt32(Y);
	if (World.IsSolidForPhysics(BX, BY, FMath::FloorToInt32(Z))) return FMath::Floor(Z) + 1;
	const double Next = Z - CubeSpec::ParachuteSpeed;
	const int32 Below = FMath::FloorToInt32(Next);
	return World.IsSolidForPhysics(BX, BY, Below) ? Below + 1 : Next;
}

ECubeFlight CubeBombs::Fly(const FCubeVoxelWorld& World, FVector& P, FVector& V)
{
	const int32 Steps = FMath::Max(1, FMath::CeilToInt32(V.Size() / 0.1));
	for (int32 I = 1; I <= Steps; I++)
	{
		const FVector N = P + V / Steps;
		if (N.X < 0 || N.X >= World.Width || N.Y < 0 || N.Y >= World.Depth || N.Z < World.MinZ) return ECubeFlight::Gone;
		if (World.IsSolid(FMath::FloorToInt32(N.X), FMath::FloorToInt32(N.Y), FMath::FloorToInt32(N.Z))) return ECubeFlight::Exploded;
		P = N;
	}
	V *= CubeSpec::ProjectileDrag;
	V.Z -= CubeSpec::ProjectileGravity;
	return ECubeFlight::Flying;
}

// ── the bomb ─────────────────────────────────────────────────────────────────────────────────────

namespace
{
	constexpr float Radius = 25.f;   // a quarter of a block

	UProceduralMeshComponent* NewPart(AActor* Owner, USceneComponent* Parent, const TCHAR* Name)
	{
		UProceduralMeshComponent* Mesh = NewObject<UProceduralMeshComponent>(Owner, Name);
		Mesh->SetupAttachment(Parent);
		Mesh->RegisterComponent();
		Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Mesh->SetCastShadow(false);
		Mesh->bUseAsyncCooking = false;
		Mesh->SetMaterial(0, FCubeShape::Material());
		return Mesh;
	}
}

ACubeBomb::ACubeBomb()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void ACubeBomb::BeginPlay()
{
	Super::BeginPlay();
	// The ball rests on the actor's origin: the band and the fuse on top, the spark at the fuse's end.
	Ball = NewPart(this, RootComponent, TEXT("Ball"));
	FCubeShape Body;
	Body.Sphere(FVector(0, 0, Radius), Radius, 16, 12, FCubeShape::Hex(TEXT("#1d1f24")));
	Body.Cylinder(FVector(0, 0, Radius * 1.95f), FVector::ZAxisVector, Radius * 0.42f, Radius * 0.3f, 12, FCubeShape::Hex(TEXT("#6b7280")));
	Body.Cylinder(FVector(Radius * 0.08f, 0, Radius * 2.3f), FQuat(FVector::YAxisVector, 0.35f).RotateVector(FVector::ZAxisVector),
		Radius * 0.08f, Radius * 0.5f, 6, FCubeShape::Hex(TEXT("#c9a26b")));
	Body.Commit(Ball);

	Spark = NewPart(this, RootComponent, TEXT("Spark"));
	FCubeShape Glow;
	Glow.bShaded = false;
	Glow.Octahedron(FVector::ZeroVector, Radius * 0.18f, FCubeShape::Hex(TEXT("#ffd166")));
	Glow.Commit(Spark);
	Spark->SetRelativeLocation(FVector(Radius * 0.2f, 0, Radius * 2.58f));

	// A canopy of eight red and white stripes over eight lines down to the bomb.
	Parachute = NewPart(this, RootComponent, TEXT("Parachute"));
	FCubeShape Canopy;
	const float Cap = PI * 0.42f, R = 110.f, CZ = 135.f;
	const FLinearColor Red = FCubeShape::Hex(TEXT("#ef4444")), White = FCubeShape::Hex(TEXT("#f8fafc")), Line = FCubeShape::Hex(TEXT("#e5e7eb"));
	Canopy.Sphere(FVector(0, 0, CZ), R, 16, 6, [&](int32 S) { return (S / 2) % 2 ? White : Red; }, Cap, true);
	const float Rim = R * FMath::Sin(Cap), RimZ = CZ + R * FMath::Cos(Cap);
	for (int32 I = 0; I < 8; I++)
	{
		const float A = I / 8.f * 2 * PI;
		Canopy.Rod(FVector(0, 0, 55), FVector(FMath::Cos(A) * Rim, FMath::Sin(A) * Rim, RimZ), 1.2f, Line);
	}
	Canopy.Commit(Parachute);
	Parachute->SetVisibility(false);
}

void ACubeBomb::Animate(double Now)
{
	if (!Spark) return;
	Spark->SetRelativeScale3D(FVector(0.7 + 0.5 * FMath::Abs(FMath::Sin(Now * 1000 / 45))));
	Spark->SetRelativeRotation(FRotator(0, FMath::RadiansToDegrees(Now * 1000 / 120), 0));
	const bool bOpen = State == TEXT("free") && !bLanded;
	Parachute->SetVisibility(bOpen);
	if (bOpen) Parachute->SetRelativeRotation(FRotator(0, 0, FMath::RadiansToDegrees(FMath::Sin(Now * 1000 / 700 + Pos.X) * 0.08)));
}

// ── explosions and smoke ─────────────────────────────────────────────────────────────────────────

namespace
{
	float Rand(float A, float B) { return FMath::FRandRange(A, B); }

	FVector Around(float Spread)
	{
		return FVector(Rand(-1, 1), Rand(-1, 1), Rand(-1, 1)).GetSafeNormal() * Rand(0.2f, 1) * Spread;
	}
}

ACubeBurst::ACubeBurst()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void ACubeBurst::Add(bool bCube, const FLinearColor& Color, const FVector& At, const FVector& Velocity, float Life, float Size, float Grow, float Gravity)
{
	UProceduralMeshComponent* Mesh = NewPart(this, RootComponent, *FString::Printf(TEXT("Particle%d"), Meshes.Num()));
	FCubeShape Shape;
	Shape.bShaded = bCube;
	if (bCube) Shape.Box(FVector::ZeroVector, FVector(0.5f), Color);
	else Shape.Sphere(FVector::ZeroVector, 1.f, 8, 5, Color);
	Shape.Commit(Mesh);
	Mesh->SetWorldLocation(At * CubeSpec::BlockCm);
	Mesh->SetWorldScale3D(FVector(Size * CubeSpec::BlockCm));
	Meshes.Add(Mesh);
	Particles.Add({ Mesh, Velocity, Life, 0, Size, Grow, Gravity });
}

void ACubeBurst::Explosion(UWorld* World, const FVector& At)
{
	ACubeBurst* B = World->SpawnActor<ACubeBurst>(At * CubeSpec::BlockCm, FRotator::ZeroRotator);
	if (!B) return;
	const TCHAR* Fire[] = { TEXT("#ffd166"), TEXT("#ff9f1c"), TEXT("#f25c05") };
	const TCHAR* Debris[] = { TEXT("#4b3621"), TEXT("#5c5c5c"), TEXT("#2f2f2f") };
	const TCHAR* Smoke[] = { TEXT("#3f3f46"), TEXT("#52525b"), TEXT("#71717a") };
	// The flash: the browser's grows twentyfold as it fades; opaque, it grows less and shrinks away instead.
	B->Add(false, FCubeShape::Hex(TEXT("#fff4c2")), At, FVector::ZeroVector, 0.18f, 0.6f, 5);
	for (int32 I = 0; I < 26; I++) B->Add(false, FCubeShape::Hex(Fire[I % 3]), At + Around(0.6f), Around(9), Rand(0.25f, 0.5f), Rand(0.35f, 0.7f), 1.2f);
	for (int32 I = 0; I < 18; I++) B->Add(true, FCubeShape::Hex(Debris[I % 3]), At, Around(12) + FVector(0, 0, 6), Rand(0.6f, 1.1f), Rand(0.1f, 0.22f), 0, 26);
	for (int32 I = 0; I < 16; I++) B->Add(false, FCubeShape::Hex(Smoke[I % 3]), At + Around(1.2f), Around(2.2f) + FVector(0, 0, 1.8f), Rand(1.2f, 2), Rand(0.5f, 0.9f), 1.1f);
}

void ACubeBurst::Smoke(UWorld* World, const FVector& At)
{
	ACubeBurst* B = World->SpawnActor<ACubeBurst>(At * CubeSpec::BlockCm, FRotator::ZeroRotator);
	if (!B) return;
	const TCHAR* Puff[] = { TEXT("#e5e7eb"), TEXT("#d4d4d8"), TEXT("#f4f4f5") };
	for (int32 I = 0; I < 10; I++) B->Add(false, FCubeShape::Hex(Puff[I % 3]), At + Around(0.25f), Around(0.6f) + FVector(0, 0, 1.1f), Rand(0.9f, 1.4f), Rand(0.12f, 0.22f), 0.5f);
}

void ACubeBurst::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const float Dt = FMath::Min(0.1f, DeltaSeconds);
	for (int32 I = Particles.Num() - 1; I >= 0; I--)
	{
		FParticle& P = Particles[I];
		P.Age += Dt;
		const float T = FMath::Min(1.f, P.Age / P.Life);
		P.Velocity.Z -= P.Gravity * Dt;
		if (P.Gravity == 0) P.Velocity *= FMath::Max(0.f, 1 - 3 * Dt);
		P.Mesh->SetWorldLocation(P.Mesh->GetComponentLocation() + P.Velocity * Dt * CubeSpec::BlockCm);
		// The browser fades a particle out; these materials are opaque, so it shrinks away as it fades.
		P.Mesh->SetWorldScale3D(FVector(P.Size * (1 + P.Grow * T) * (1 - T) * CubeSpec::BlockCm));
		if (T < 1) continue;
		P.Mesh->DestroyComponent();
		Particles.RemoveAt(I);
	}
	if (Particles.Num() == 0) Destroy();
}
