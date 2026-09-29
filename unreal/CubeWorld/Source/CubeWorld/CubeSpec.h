// The canonical numbers, all from the Minecraft Wiki (../../SPEC.md names the page for each one).
// Mirrors CubeWorld.Server/Spec.cs and web/spec.js. Distances in blocks, velocities in blocks per tick;
// the world's coordinates are the server's: x and y on the ground, z up. One block is 100 cm in Unreal.
// The server and the client share this file: the dedicated server judges every action by these numbers, the
// client simulates its own movement with them.
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
	// Reach: 4.5 blocks for blocks, 3 for entities; the server allows a little extra for latency.
	constexpr double BlockReach = 4.5, EntityReach = 3.0, ReachTolerance = 1.0;
	constexpr double MaxHealth = 20, KnockbackLift = 0.4, Push = 0.05;
	constexpr int32 HurtTicks = 10, DigCooldownTicks = 5;
	// Field of view 70° is Minecraft's VERTICAL angle (Options); Unreal's camera takes the horizontal one, so
	// the pawn converts with the viewport's aspect. Sprinting scales the angle by 1.15.
	constexpr float Fov = 70.f, SprintFov = 1.15f;
	// The mouse turns the view 0.15° per pixel at Minecraft's default sensitivity (Options § Mouse sensitivity).
	constexpr float DegreesPerMousePixel = 0.15f;

	// Health 20; 10 ticks of invulnerability after a hit; natural regeneration of 1 every 80 ticks.
	constexpr int32 InvulnerabilityTicks = 10, RegenIntervalTicks = 80;
	// An empty hand deals 1 damage, recharges in 5 ticks (attack speed 4) and knocks back 0.4; sprinting adds 0.5.
	constexpr double FistDamage = 1.0, Knockback = 0.4, SprintKnockback = 0.5;
	constexpr int32 FistChargeTicks = 5;
	// Fall damage: 1 per block fallen beyond the third.
	constexpr double SafeFallDistance = 3.0;
	constexpr int32 StackSize = 64, StartingStack = 64;

	constexpr float BlockCm = 100.f;
	constexpr int32 TextureSize = 16;
	constexpr float ModelScale = 0.9375f;

	// Six regions, three across and two deep, like the six of a die: the upper row is the C# servers', the lower the
	// Unreal servers' (each prefers its own row and takes any free region when its row is full).
	constexpr int32 RegionSize = 24, RegionColumns = 3, RegionRows = 2, Width_ = RegionSize * RegionColumns, Depth = RegionSize * RegionRows, MinZ = -4, MaxZ = 64, RegionCount = RegionColumns * RegionRows;

	// Bombs (CubeWorld.Server/Spec.cs): thrown at 1 block a tick, drag 0.99, gravity 0.05 (a thrown potion's);
	// a parachute comes down at 0.1 a tick from 32 up. It explodes with a creeper's power of 3.
	constexpr double ThrowSpeed = 1.0, ProjectileDrag = 0.99, ProjectileGravity = 0.05, ParachuteSpeed = 0.1, BombPower = 3;
	constexpr int32 BombFlightTicks = 200, OwnerImmunityTicks = 4;
	// A player picks a bomb up as Minecraft players pick up items: within the hitbox grown by 1 sideways, 0.5 up and down.
	constexpr double DropHeight = 32, PickupReach = 1.0, PickupReachUp = 0.5;
	// The blast hurts players within 3 blocks, half of Minecraft's 2 × power. It breaks blocks with a power of 1, not 3:
	// the block under it and one around, a 3 × 3 patch of the top layer on flat ground.
	constexpr double BlastReach = 3, CraterPower = 1;

	/** The region a spot belongs to: region r is column r % RegionColumns of row r / RegionColumns. */
	inline int32 RegionOf(double X, double Y) { return FMath::FloorToInt32(Y / RegionSize) * RegionColumns + FMath::FloorToInt32(X / RegionSize); }
	/** The blocks of a region: x in [X0, X1), y in [Y0, Y1). */
	inline void RegionBounds(int32 Region, int32& X0, int32& X1, int32& Y0, int32& Y1)
	{
		X0 = Region % RegionColumns * RegionSize; X1 = X0 + RegionSize; Y0 = Region / RegionColumns * RegionSize; Y1 = Y0 + RegionSize;
	}
	/** Where a region's players spawn: its middle. */
	inline FVector2D RegionCentre(int32 Region) { return FVector2D((Region % RegionColumns + 0.5) * RegionSize, (Region / RegionColumns + 0.5) * RegionSize); }
	/** The room types the world's servers register under: the C# servers' and the Unreal servers'. */
	inline const TArray<FString>& RoomTypes() { static TArray<FString> Types = { TEXT("cubeworld"), TEXT("cubeworld-ue") }; return Types; }
	inline const TCHAR* RegionColorName(int32 Region)
	{
		static const TCHAR* Colors[] = { TEXT("red"), TEXT("blue"), TEXT("green"), TEXT("yellow"), TEXT("purple"), TEXT("pink") };
		return Region >= 0 && Region < RegionCount ? Colors[Region] : TEXT("grey");
	}

	/** Minecraft's yaw is Unreal's yaw minus 90°; its pitch is positive looking down. Both in radians. */
	inline double YawFromUnreal(float UnrealYawDegrees) { return FMath::DegreesToRadians(FRotator::NormalizeAxis(UnrealYawDegrees - 90.f)); }
	inline double PitchFromUnreal(float UnrealPitchDegrees) { return -FMath::DegreesToRadians(FRotator::NormalizeAxis(UnrealPitchDegrees)); }
}

/** One block kind and what Minecraft gives it. */
struct FBlockDef
{
	FName Kind;
	double Hardness = 0;
	bool bNeedsTool = false;
	bool bTransparent = true;
	bool bGravity = false;
	FName Drop;
	double BlastResistance = 0;
	/** Ticks to break by hand: damage per tick is 1/(hardness·30), or 1/(hardness·100) when a tool is needed. -1 for unbreakable. */
	int32 BreakTicks = -1;

	bool IsSolid() const { return Kind != NAME_None && Kind != TEXT("air"); }
	bool IsBreakable() const { return Hardness >= 0; }
	bool IsPlaceable() const { return IsSolid() && Kind != TEXT("bedrock"); }
};

namespace CubeSpec
{
	/** The block registry, in hotbar order after air and bedrock. Mirrors Spec.Blocks on the C# server. */
	inline const TArray<FBlockDef>& Blocks()
	{
		static TArray<FBlockDef> All = []()
		{
			struct FRow { const TCHAR* Kind; double Hardness; bool bNeedsTool; bool bTransparent; bool bGravity; const TCHAR* Drop; double Blast; };
			const FRow Rows[] = {
				{ TEXT("air"), 0, false, true, false, nullptr, 0 },
				{ TEXT("bedrock"), -1, true, false, false, nullptr, 3600000 },
				{ TEXT("grass"), 0.6, false, false, false, TEXT("dirt"), 0.6 },
				{ TEXT("dirt"), 0.5, false, false, false, TEXT("dirt"), 0.5 },
				{ TEXT("sand"), 0.5, false, false, true, TEXT("sand"), 0.5 },
				{ TEXT("stone"), 1.5, true, false, false, nullptr, 6 },
				{ TEXT("wood"), 2.0, false, false, false, TEXT("wood"), 2 },
				{ TEXT("brick"), 2.0, true, false, false, nullptr, 6 },
				{ TEXT("glass"), 0.3, false, true, false, nullptr, 0.3 },
				{ TEXT("gold"), 3.0, true, false, false, nullptr, 6 },
				{ TEXT("leaves"), 0.2, false, true, false, nullptr, 0.2 },
			};
			TArray<FBlockDef> Out;
			for (const FRow& R : Rows)
			{
				FBlockDef B;
				B.Kind = FName(R.Kind); B.Hardness = R.Hardness; B.bNeedsTool = R.bNeedsTool; B.bTransparent = R.bTransparent; B.bGravity = R.bGravity;
				B.Drop = R.Drop ? FName(R.Drop) : NAME_None; B.BlastResistance = R.Blast;
				B.BreakTicks = B.IsBreakable() ? FMath::CeilToInt32(R.Hardness * (R.bNeedsTool ? 100 : 30) - 1e-9) : -1;
				Out.Add(B);
			}
			return Out;
		}();
		return All;
	}

	inline const FBlockDef& Block(FName Kind)
	{
		for (const FBlockDef& B : Blocks()) if (B.Kind == Kind) return B;
		return Blocks()[0];
	}

	/** A block kind as one byte on the wire: its index in Blocks(). Unknown kinds are air. */
	inline uint8 KindIndex(FName Kind)
	{
		const TArray<FBlockDef>& All = Blocks();
		for (int32 I = 0; I < All.Num(); I++) if (All[I].Kind == Kind) return (uint8)I;
		return 0;
	}

	inline FName KindOf(uint8 Index) { return Blocks().IsValidIndex(Index) ? Blocks()[Index].Kind : Blocks()[0].Kind; }

	/** Hotbar order: every placeable kind. */
	inline const TArray<FName>& Hotbar()
	{
		static TArray<FName> Kinds = []() { TArray<FName> Out; for (const FBlockDef& B : Blocks()) if (B.IsPlaceable()) Out.Add(B.Kind); return Out; }();
		return Kinds;
	}

	/** Superflat "Classic Flat": one bedrock, two dirt, one grass block. The player stands at z = 0. */
	inline const TMap<int32, FName>& Layers()
	{
		static TMap<int32, FName> L = { { -4, TEXT("bedrock") }, { -3, TEXT("dirt") }, { -2, TEXT("dirt") }, { -1, TEXT("grass") } };
		return L;
	}
}

/** Where the oaks stand (ground x, y): four per region, clear of the spawn. Mirrors Spec.Trees on the server. */
inline TArray<FIntPoint> CubeTreeSpots()
{
	TArray<FIntPoint> Spots;
	for (int32 R = 0; R < CubeSpec::RegionCount; R++)
	{
		int32 X0, X1, Y0, Y1;
		CubeSpec::RegionBounds(R, X0, X1, Y0, Y1);
		Spots.Add(FIntPoint(X0 + 4, Y0 + 5));
		Spots.Add(FIntPoint(X0 + 18, Y0 + 4));
		Spots.Add(FIntPoint(X0 + 6, Y0 + 18));
		Spots.Add(FIntPoint(X0 + 19, Y0 + 17));
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
