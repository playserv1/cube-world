// A copy of the world: a superflat terrain and the oaks generated on the fly, plus the records that override
// them (kind "air" is a dug-out generated block). The client draws from it and moves through it, the server judges
// by it. Same rules as the C# server's World.cs and the browser's voxels.js; coordinates are the server's
// (x, y on the ground, z up).
#pragma once

#include "CoreMinimal.h"
#include "CubeSpec.h"

struct FCubeRayHit
{
	FIntVector Block;
	FIntVector Normal;
	double Distance = 0;
};

class FCubeVoxelWorld
{
public:
	static constexpr int32 Chunk = 16;

	int32 Width = CubeSpec::Width_, Depth = CubeSpec::Depth, MinZ = CubeSpec::MinZ, MaxZ = CubeSpec::MaxZ, RegionSize = CubeSpec::RegionSize;
	TArray<FName> RegionColors = { TEXT("red"), TEXT("blue"), TEXT("green"), TEXT("yellow"), TEXT("purple"), TEXT("pink") };

	FCubeVoxelWorld();

	const FBlockDef& Block(FName Kind) const;
	bool Inside(int32 X, int32 Y, int32 Z) const { return X >= 0 && X < Width && Y >= 0 && Y < Depth && Z >= MinZ && Z < MaxZ; }
	FName Generated(int32 X, int32 Y, int32 Z) const;
	FName KindAt(int32 X, int32 Y, int32 Z) const;
	bool IsSolid(int32 X, int32 Y, int32 Z) const { return Inside(X, Y, Z) && Block(KindAt(X, Y, Z)).IsSolid(); }
	/** What the player's body collides with: blocks, and the world border and floor as walls. */
	bool IsSolidForPhysics(int32 X, int32 Y, int32 Z) const;
	FName RegionColor(int32 X, int32 Y) const;

	/** Sets an override (None restores the generated block). Returns the chunk ids to rebuild. */
	TSet<FIntPoint> Set(int32 X, int32 Y, int32 Z, FName Kind);
	TSet<FIntPoint> Hide(int32 X, int32 Y, int32 Z, bool bOn);
	TSet<FIntPoint> ChunksAround(int32 X, int32 Y) const;
	TArray<FIntPoint> AllChunks() const;
	void Clear() { Overrides.Empty(); Hidden.Empty(); }
	const TMap<FIntVector, FName>& GetOverrides() const { return Overrides; }
	/** Replaces every changed block at once and returns the blocks that differ: a snapshot applied without a rebuild of all. */
	TArray<FIntVector> ReplaceOverrides(const TMap<FIntVector, FName>& Next)
	{
		TArray<FIntVector> Changed;
		for (const auto& P : Overrides) { const FName* N = Next.Find(P.Key); if (!N || *N != P.Value) Changed.Add(P.Key); }
		for (const auto& P : Next) if (!Overrides.Contains(P.Key)) Changed.Add(P.Key);
		Overrides = Next;
		return Changed;
	}

	/** Walks the ray block by block (Amanatides & Woo) and returns the first solid block and the face it entered. */
	bool Raycast(const FVector& Origin, const FVector& Direction, double Reach, FCubeRayHit& OutHit) const;

	TMap<FName, FBlockDef> Blocks;

private:
	TMap<int32, FName> Layers;
	TMap<FIntVector, FName> Trees;
	TMap<FIntVector, FName> Overrides;
	TSet<FIntVector> Hidden;
	FBlockDef Air;
};
