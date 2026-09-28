// Bombs, as web/bombs.js and web/bombfx.js draw them. A free bomb comes down under its parachute, a held one
// sits in its holder's hand, a thrown one flies the path the server flies it; the server says when one is
// picked up, thrown, explodes or fizzles out. A black ball with a fuse and a flickering spark, a striped
// parachute, an explosion of fire, debris and smoke, and the light puff of a bomb that fizzles out.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CubeBombs.generated.h"

class FCubeVoxelWorld;
class UProceduralMeshComponent;

/** A "bomb" frame: the WorldBomb record, how long ago it entered its state, and the height of a free one now. */
struct FCubeBombFrame
{
	FString Id, State, Holder;
	double X = 0, Y = 0, Z = 0, VX = 0, VY = 0, VZ = 0;
	double Age = 0;
	TOptional<double> Height;
};

enum class ECubeFlight : uint8 { Flying, Exploded, Gone };

namespace CubeBombs
{
	/** One tick under the parachute: down 0.1 until the bomb rests on a block; out on top of a block put on it. */
	double Descend(const FCubeVoxelWorld& World, double X, double Y, double Z);
	/** One tick of a thrown bomb, the server's Bomb.Fly without the players: the server says whom it hits. */
	ECubeFlight Fly(const FCubeVoxelWorld& World, FVector& P, FVector& V);
}

UCLASS()
class CUBEWORLD_API ACubeBomb : public AActor
{
	GENERATED_BODY()

public:
	ACubeBomb();
	virtual void BeginPlay() override;

	/** The spark flickers, the parachute sways while it is open. */
	void Animate(double Now);

	FString Id, State, Holder;
	/** Where the simulation has it, in blocks, and where it was a tick before. */
	FVector Pos = FVector::ZeroVector, Prev = FVector::ZeroVector, Vel = FVector::ZeroVector;
	bool bLanded = false;
	/** The tick a thrown bomb came to rest on this client, 0 while it still flies. */
	int64 Stopped = 0;

	UPROPERTY() UProceduralMeshComponent* Ball = nullptr;
	UPROPERTY() UProceduralMeshComponent* Spark = nullptr;
	UPROPERTY() UProceduralMeshComponent* Parachute = nullptr;
};

/** An explosion or a puff of smoke: a handful of particles that fly, grow and fade, then the actor goes. */
UCLASS()
class CUBEWORLD_API ACubeBurst : public AActor
{
	GENERATED_BODY()

public:
	ACubeBurst();
	virtual void Tick(float DeltaSeconds) override;

	/** At is in blocks. */
	static void Explosion(UWorld* World, const FVector& At);
	/** A bomb that is not needed any more: a small light-grey puff that rises and thins out. Nothing is hurt. */
	static void Smoke(UWorld* World, const FVector& At);

private:
	struct FParticle
	{
		UProceduralMeshComponent* Mesh = nullptr;
		FVector Velocity;
		float Life = 1, Age = 0, Size = 1, Grow = 0, Gravity = 0;
	};

	void Add(bool bCube, const FLinearColor& Color, const FVector& At, const FVector& Velocity, float Life, float Size, float Grow = 0, float Gravity = 0);

	TArray<FParticle> Particles;
	UPROPERTY() TArray<UProceduralMeshComponent*> Meshes;
};
