// The world on screen: one procedural mesh per 16 × 16 chunk with hidden faces culled and faces lit as
// Minecraft lights them (top 1, sides 0.8 and 0.6, bottom 0.5), glass and leaves as cutouts, cracks
// over blocks being dug, falling sand, and a blue box for the sky.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CubeWorldActor.generated.h"

class UProceduralMeshComponent;
class UMaterialInstanceDynamic;
class UCubeWorldGameInstance;
class FCubeVoxelWorld;
class FCubeTextures;
struct FCubePose;

struct FCubeFalling
{
	UProceduralMeshComponent* Mesh = nullptr;
	int32 X, Y, ToZ;
	double Z, VZ;
};

UCLASS()
class CUBEWORLD_API ACubeWorldActor : public AActor
{
	GENERATED_BODY()

public:
	ACubeWorldActor();
	virtual void BeginPlay() override;
	/** Loads the materials, the sky and every chunk's mesh; called as soon as the map is up, and again (no-op) at BeginPlay. */
	void Init();
	virtual void Tick(float DeltaSeconds) override;

	/** Materials for everything drawn from the atlas; the pawn borrows them for the held-block icon. */
	UPROPERTY() UMaterialInstanceDynamic* Opaque = nullptr;
	UPROPERTY() UMaterialInstanceDynamic* Cutout = nullptr;
	UPROPERTY() UMaterialInstanceDynamic* Overlay = nullptr;
	UPROPERTY() UMaterialInstanceDynamic* Unlit = nullptr;

private:
	void RebuildAll();
	void RebuildAllFromWelcome(const FCubePose& You, bool bTeleport);
	void Rebuild(const TSet<FIntPoint>& Chunks);
	void RebuildChunk(const FIntPoint& Id);
	void BuildBlockMesh(UProceduralMeshComponent* Mesh, FName Kind, FName Tile, bool bCrack);
	void HandleCubes(const TArray<FIntVector>& Changed);
	void HandleFall(FName Kind, int32 X, int32 Y, int32 FromZ, int32 ToZ);
	void HandleDig(const FString& PlayerId, int32 X, int32 Y, int32 Z, int32 Stage);
	void TickFalling();
	void BuildSky();
	UProceduralMeshComponent* NewMesh(const FString& Name);

	UPROPERTY() TMap<FIntPoint, UProceduralMeshComponent*> Chunks;
	UPROPERTY() TMap<FString, UProceduralMeshComponent*> Cracks;
	TArray<FCubeFalling> Falling;
	UCubeWorldGameInstance* Game = nullptr;
	float Accumulator = 0;
	int32 MeshCounter = 0;
};
