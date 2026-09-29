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
	constexpr float Radius = 25.f;   // a quarter of a block: the head is half a block wide

	// The creeper head as web/bombfx.js paints it: 8 × 8 × 8 pixels of mottled green, the face on one side.
	const TCHAR* Greens[] = { TEXT("#4c9a3a"), TEXT("#5cb247"), TEXT("#6fc452"), TEXT("#3f8a31"), TEXT("#85d16b") };
	const TCHAR* Face[] = {
		TEXT("........"),
		TEXT("........"),
		TEXT(".##..##."),
		TEXT(".##..##."),
		TEXT("...##..."),
		TEXT("..####.."),
		TEXT("..####.."),
		TEXT("..#..#.."),
	};

	FLinearColor Pixel(int32 X, int32 Y, bool bFace)
	{
		if (bFace && Face[Y][X] == '#') return FCubeShape::Hex(TEXT("#101410"));
		return FCubeShape::Hex(Greens[(X * 7 + Y * 13 + (bFace ? 3 : 0)) % UE_ARRAY_COUNT(Greens)]);
	}

	/** One side of the head around the origin, 8 × 8 pixels from the corner Origin, Right and Down one pixel each. */
	void Side(FCubeShape& Shape, const FVector& Origin, const FVector& Right, const FVector& Down, bool bFace)
	{
		const FVector Out = (Origin + (Right + Down) * 4).GetSafeNormal();
		for (int32 Y = 0; Y < 8; Y++)
			for (int32 X = 0; X < 8; X++)
			{
				const FVector A = Origin + Right * X + Down * Y;
				Shape.Quad(A, A + Right, A + Right + Down, A + Down, Out, Pixel(X, Y, bFace));
			}
	}

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
	// The head rests on the actor's origin, its face looking along −X, at the holder's camera.
	Head = NewPart(this, RootComponent, TEXT("Head"));
	FCubeShape Body;
	const float S = Radius * 2 / 8, H = Radius;
	const FVector X = FVector::XAxisVector * S, Y = FVector::YAxisVector * S, Z = FVector::ZAxisVector * S;
	Side(Body, FVector(-H, -H, H), Y, -Z, true);     // the face, −X
	Side(Body, FVector(H, -H, H), Y, -Z, false);     // the back, +X
	Side(Body, FVector(-H, -H, H), X, -Z, false);    // −Y
	Side(Body, FVector(H, H, H), -X, -Z, false);     // +Y
	Side(Body, FVector(H, H, H), -Y, -X, false);     // the top
	Side(Body, FVector(-H, H, -H), -Y, X, false);    // the bottom
	Body.Commit(Head);
	Head->SetRelativeLocation(FVector(0, 0, Radius));

	// The white a creeper flashes before it blows, a shade bigger than the head; Animate shows it in turns.
	Flash = NewPart(this, Head, TEXT("Flash"));
	FCubeShape White;
	White.bShaded = false;
	White.Box(FVector::ZeroVector, FVector(Radius * 1.02f), FCubeShape::Hex(TEXT("#f4f4f5")));
	White.Commit(Flash);
	Flash->SetVisibility(false);

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
	if (!Head) return;
	// The browser tints the head white and back; these materials are unlit vertex colours, so the white shell
	// shows at the top of each swell.
	const double Swell = FMath::Max(0.0, FMath::Sin(Now * 1000 / 160));
	Head->SetRelativeScale3D(FVector(1 + 0.06 * Swell));
	Flash->SetVisibility(Swell > 0.6);
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
