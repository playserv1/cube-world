// The canonical numbers, all from the Minecraft Wiki (../../SPEC.md names the page for each one).
// Mirrors CubeWorld.Server/Spec.cs and web/spec.js. Distances in blocks, velocities in blocks per tick;
// the world's coordinates are the server's: x and y on the ground, z up. One block is 100 cm in Unreal.
#pragma once

#include "CoreMinimal.h"

namespace CubeSpec
{
	constexpr int32 TicksPerSecond = 20;
	constexpr float TickSeconds = 1.0f / TicksPerSecond;

	constexpr double Gravity = 0.08, VerticalDrag = 0.98, JumpVelocity = 0.42, SprintJumpBoost = 0.2;
	constexpr int32 JumpDelayTicks = 10;
	constexpr double WalkAcceleration = 0.1, AirAcceleration = 0.02, SprintMultiplier = 1.3, SneakMultiplier = 0.3;
	constexpr double InputScale = 0.98, GroundFriction = 0.91 * 0.6, AirFriction = 0.91, MinVelocity = 0.003;

	constexpr double Width = 0.6, Height = 1.8, SneakHeight = 1.5, EyeHeight = 1.62, SneakEyeHeight = 1.27, StepHeight = 0.6;
	constexpr double BlockReach = 4.5, EntityReach = 3.0;
	constexpr double MaxHealth = 20, KnockbackLift = 0.4, Push = 0.05;
	constexpr int32 HurtTicks = 10, DigCooldownTicks = 5;
	// Field of view 70° is Minecraft's VERTICAL angle (Options); Unreal's camera takes the horizontal one, so
	// the pawn converts with the viewport's aspect. Sprinting scales the angle by 1.15.
	constexpr float Fov = 70.f, SprintFov = 1.15f;
	// The mouse turns the view 0.15° per pixel at Minecraft's default sensitivity (Options § Mouse sensitivity).
	constexpr float DegreesPerMousePixel = 0.15f;

	constexpr float BlockCm = 100.f;
	constexpr int32 TextureSize = 16;
	constexpr float ModelScale = 0.9375f;

	constexpr int32 RegionSize = 24, Width_ = 72, Depth = 24, MinZ = -4, MaxZ = 64;

	// Bombs (CubeWorld.Server/Spec.cs): thrown at 1 block a tick, drag 0.99, gravity 0.05 (a thrown potion's);
	// a parachute comes down at 0.1 a tick from 32 up. The server decides where one goes; the client flies the same path to draw it.
	constexpr double ThrowSpeed = 1.0, ProjectileDrag = 0.99, ProjectileGravity = 0.05, ParachuteSpeed = 0.1;
	constexpr int32 BombFlightTicks = 200;
}

/** One block kind and what Minecraft gives it; the server sends the same table in its welcome frame. */
struct FBlockDef
{
	FName Kind;
	double Hardness = 0;
	bool bNeedsTool = false;
	bool bTransparent = true;
	bool bGravity = false;
	FName Drop;
	int32 BreakTicks = -1;

	bool IsSolid() const { return Kind != NAME_None && Kind != TEXT("air"); }
};

/** Where the oaks stand (ground x, y): four per region, clear of the spawn. Mirrors Spec.Trees on the server. */
inline TArray<FIntPoint> CubeTreeSpots()
{
	TArray<FIntPoint> Spots;
	for (int32 R = 0; R < 3; R++)
	{
		Spots.Add(FIntPoint(R * 24 + 4, 5));
		Spots.Add(FIntPoint(R * 24 + 18, 4));
		Spots.Add(FIntPoint(R * 24 + 6, 18));
		Spots.Add(FIntPoint(R * 24 + 19, 17));
	}
	return Spots;
}

/** An oak: five logs, two 5 × 5 leaf layers without corners around the top two logs, a 3 × 3 layer and a cross on top. */
inline TMap<FIntVector, FName> CubeBuildTrees(const TArray<FIntPoint>& Spots)
{
	TMap<FIntVector, FName> Blocks;
	const FName Wood(TEXT("wood")), Leaves(TEXT("leaves"));
	for (const FIntPoint& T : Spots)
	{
		for (int32 Dz = 0; Dz < 5; Dz++) Blocks.Add(FIntVector(T.X, T.Y, Dz), Wood);
		for (int32 Dx = -2; Dx <= 2; Dx++)
			for (int32 Dy = -2; Dy <= 2; Dy++)
			{
				const bool bCorner = FMath::Abs(Dx) == 2 && FMath::Abs(Dy) == 2, bTrunk = Dx == 0 && Dy == 0;
				for (int32 Dz = 3; Dz <= 4; Dz++)
					if (!bCorner && !bTrunk && !Blocks.Contains(FIntVector(T.X + Dx, T.Y + Dy, Dz))) Blocks.Add(FIntVector(T.X + Dx, T.Y + Dy, Dz), Leaves);
				if (FMath::Abs(Dx) <= 1 && FMath::Abs(Dy) <= 1 && !Blocks.Contains(FIntVector(T.X + Dx, T.Y + Dy, 5))) Blocks.Add(FIntVector(T.X + Dx, T.Y + Dy, 5), Leaves);
				if (FMath::Abs(Dx) + FMath::Abs(Dy) <= 1 && !Blocks.Contains(FIntVector(T.X + Dx, T.Y + Dy, 6))) Blocks.Add(FIntVector(T.X + Dx, T.Y + Dy, 6), Leaves);
			}
	}
	return Blocks;
}
