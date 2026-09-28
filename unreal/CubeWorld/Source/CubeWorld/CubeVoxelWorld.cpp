#include "CubeVoxelWorld.h"

FCubeVoxelWorld::FCubeVoxelWorld()
{
	Air.Kind = TEXT("air");
	Layers = { { -4, TEXT("bedrock") }, { -3, TEXT("dirt") }, { -2, TEXT("dirt") }, { -1, TEXT("grass") } };
	Trees = CubeBuildTrees(CubeTreeSpots());
}

void FCubeVoxelWorld::Configure(int32 InWidth, int32 InDepth, int32 InMinZ, int32 InMaxZ, int32 InRegionSize,
	const TMap<int32, FName>& InLayers, const TArray<FIntPoint>& InTrees, const TArray<FBlockDef>& InBlocks)
{
	Width = InWidth; Depth = InDepth; MinZ = InMinZ; MaxZ = InMaxZ; RegionSize = InRegionSize;
	Layers = InLayers;
	Trees = CubeBuildTrees(InTrees.Num() ? InTrees : CubeTreeSpots());
	Blocks.Empty();
	for (const FBlockDef& B : InBlocks) Blocks.Add(B.Kind, B);
	Clear();
}

const FBlockDef& FCubeVoxelWorld::Block(FName Kind) const
{
	const FBlockDef* Found = Blocks.Find(Kind);
	return Found ? *Found : Air;
}

FName FCubeVoxelWorld::Generated(int32 X, int32 Y, int32 Z) const
{
	if (!Inside(X, Y, Z)) return Air.Kind;
	if (Z >= 0) { const FName* T = Trees.Find(FIntVector(X, Y, Z)); return T ? *T : Air.Kind; }
	const FName* L = Layers.Find(Z);
	return L ? *L : Air.Kind;
}

FName FCubeVoxelWorld::KindAt(int32 X, int32 Y, int32 Z) const
{
	const FIntVector Key(X, Y, Z);
	if (Hidden.Contains(Key)) return Air.Kind;
	const FName* O = Overrides.Find(Key);
	return O ? *O : Generated(X, Y, Z);
}

bool FCubeVoxelWorld::IsSolidForPhysics(int32 X, int32 Y, int32 Z) const
{
	if (X < 0 || X >= Width || Y < 0 || Y >= Depth || Z < MinZ) return true;
	return IsSolid(X, Y, Z);
}

FName FCubeVoxelWorld::RegionColor(int32 X) const
{
	const int32 R = FMath::FloorToInt32((float)X / RegionSize);
	return RegionColors.IsValidIndex(R) ? RegionColors[R] : FName(TEXT("green"));
}

TSet<FIntPoint> FCubeVoxelWorld::Set(int32 X, int32 Y, int32 Z, FName Kind)
{
	const FIntVector Key(X, Y, Z);
	if (Kind == NAME_None) Overrides.Remove(Key); else Overrides.Add(Key, Kind);
	return ChunksAround(X, Y);
}

TSet<FIntPoint> FCubeVoxelWorld::Hide(int32 X, int32 Y, int32 Z, bool bOn)
{
	const FIntVector Key(X, Y, Z);
	if (bOn) Hidden.Add(Key); else Hidden.Remove(Key);
	return ChunksAround(X, Y);
}

TSet<FIntPoint> FCubeVoxelWorld::ChunksAround(int32 X, int32 Y) const
{
	TSet<FIntPoint> Ids;
	const int32 Offsets[5][2] = { { 0, 0 }, { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
	for (const auto& O : Offsets)
	{
		const int32 CX = FMath::FloorToInt32((float)(X + O[0]) / Chunk), CY = FMath::FloorToInt32((float)(Y + O[1]) / Chunk);
		if (CX >= 0 && CY >= 0 && CX * Chunk < Width && CY * Chunk < Depth) Ids.Add(FIntPoint(CX, CY));
	}
	return Ids;
}

TArray<FIntPoint> FCubeVoxelWorld::AllChunks() const
{
	TArray<FIntPoint> Ids;
	for (int32 CX = 0; CX * Chunk < Width; CX++)
		for (int32 CY = 0; CY * Chunk < Depth; CY++) Ids.Add(FIntPoint(CX, CY));
	return Ids;
}

bool FCubeVoxelWorld::Raycast(const FVector& Origin, const FVector& Direction, double Reach, FCubeRayHit& OutHit) const
{
	int32 P[3] = { FMath::FloorToInt32(Origin.X), FMath::FloorToInt32(Origin.Y), FMath::FloorToInt32(Origin.Z) };
	const double O[3] = { Origin.X, Origin.Y, Origin.Z }, D[3] = { Direction.X, Direction.Y, Direction.Z };
	int32 Step[3]; double Delta[3], Next[3];
	for (int32 A = 0; A < 3; A++)
	{
		Step[A] = D[A] > 0 ? 1 : (D[A] < 0 ? -1 : 0);
		Delta[A] = D[A] != 0 ? FMath::Abs(1 / D[A]) : TNumericLimits<double>::Max();
		Next[A] = D[A] > 0 ? (P[A] + 1 - O[A]) * Delta[A] : (D[A] < 0 ? (O[A] - P[A]) * Delta[A] : TNumericLimits<double>::Max());
	}
	FIntVector Face(0, 0, 0);
	bool bHasFace = false;
	double Travelled = 0;
	for (int32 I = 0; I < 64; I++)
	{
		if (bHasFace && IsSolid(P[0], P[1], P[2]))
		{
			OutHit.Block = FIntVector(P[0], P[1], P[2]);
			OutHit.Normal = Face;
			OutHit.Distance = Travelled;
			return true;
		}
		const int32 Axis = Next[0] < Next[1] ? (Next[0] < Next[2] ? 0 : 2) : (Next[1] < Next[2] ? 1 : 2);
		Travelled = Next[Axis];
		if (Travelled > Reach) return false;
		Next[Axis] += Delta[Axis];
		P[Axis] += Step[Axis];
		Face = FIntVector(0, 0, 0);
		Face[Axis] = -Step[Axis];
		bHasFace = true;
	}
	return false;
}
