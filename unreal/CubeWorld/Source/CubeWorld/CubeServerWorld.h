// The world as the dedicated server judges it: the superflat terrain and the oaks generated on the fly, plus the
// records that override them. Blocks are placed against a face and broken one at a time, gravity blocks fall when
// nothing holds them, an explosion is Minecraft's ray march. A port of CubeWorld.Server/World.cs; the geometry
// queries are the client's FCubeVoxelWorld, so both sides agree on every block.
#pragma once

#include "CoreMinimal.h"
#include "CubeSpec.h"
#include "CubeVoxelWorld.h"
#include "UObject/StrongObjectPtr.h"

#include "CubeEntities.h"

/** A record that overrides the terrain, and the platform row that holds it once one exists. */
struct FCubeOverride
{
	FName Kind;
	FString By, On;
	int64 At = 0;
	/** The world's Version when this block was last set, heard or written. */
	uint64 Version = 0;
	TStrongObjectPtr<UWorldCube> Row;
};

struct FCubeChange
{
	FIntVector At;
	FName Kind;
	FString By, On;
};

struct FCubeFall
{
	FName Kind;
	int32 X = 0, Y = 0, FromZ = 0, ToZ = 0;
};

struct FCubeWorldUpdate
{
	TArray<FCubeChange> Changes;
	TArray<FCubeFall> Falls;
};

/** A player's hitbox: feet at (X, Y, Z), CubeSpec::Width wide, Height tall. */
struct FCubeHitbox
{
	double X = 0, Y = 0, Z = 0, Height = CubeSpec::Height;
};

/**
 * .NET's seeded Random (Knuth's subtractive generator, as System.Random keeps it for a given seed), so a crater
 * worked out here from a bomb's seed is the crater the C# servers work out from it.
 */
class FDotNetRandom
{
public:
	explicit FDotNetRandom(int32 Seed)
	{
		const int32 Subtraction = Seed == MIN_int32 ? MAX_int32 : FMath::Abs(Seed);
		int32 Mj = MSeed - Subtraction, Mk = 1, Ii = 0;
		SeedArray[55] = Mj;
		for (int32 I = 1; I < 55; I++)
		{
			if ((Ii += 21) >= 55) Ii -= 55;
			SeedArray[Ii] = Mk;
			Mk = Mj - Mk;
			if (Mk < 0) Mk += MBig;
			Mj = SeedArray[Ii];
		}
		for (int32 K = 1; K < 5; K++)
			for (int32 I = 1; I < 56; I++)
			{
				int32 N = I + 30;
				if (N >= 55) N -= 55;
				SeedArray[I] -= SeedArray[1 + N];
				if (SeedArray[I] < 0) SeedArray[I] += MBig;
			}
		INext = 0;
		INextP = 21;
	}

	/** [0, 1), as Random.NextDouble. */
	double NextDouble() { return Sample() * (1.0 / MBig); }

private:
	static constexpr int32 MBig = MAX_int32, MSeed = 161803398;
	int32 SeedArray[56] = {};
	int32 INext = 0, INextP = 21;

	int32 Sample()
	{
		int32 LocINext = INext, LocINextP = INextP;
		if (++LocINext >= 56) LocINext = 1;
		if (++LocINextP >= 56) LocINextP = 1;
		int32 Result = SeedArray[LocINext] - SeedArray[LocINextP];
		if (Result == MBig) Result--;
		if (Result < 0) Result += MBig;
		SeedArray[LocINext] = Result;
		INext = LocINext;
		INextP = LocINextP;
		return Result;
	}
};

class FCubeServerWorld
{
public:
	/** The geometry: what is solid where. Kept in step with Overrides. */
	FCubeVoxelWorld Voxels;
	TMap<FIntVector, FCubeOverride> Overrides;
	/** Counts every block set, heard or written: a read of the table knows what changed after it began. */
	uint64 Version = 0;

	/** The columns of a region: From is its first, To the next region's first. */
	static bool Inside(int32 X, int32 Y, int32 Z) { return X >= 0 && X < CubeSpec::Width_ && Y >= 0 && Y < CubeSpec::Depth && Z >= CubeSpec::MinZ && Z < CubeSpec::MaxZ; }
	static FString Key(int32 X, int32 Y, int32 Z) { return FString::Printf(TEXT("%d:%d:%d"), X, Y, Z); }

	const FBlockDef& BlockAt(int32 X, int32 Y, int32 Z) const { return Voxels.Block(Voxels.KindAt(X, Y, Z)); }
	bool IsSolid(int32 X, int32 Y, int32 Z) const { return Voxels.IsSolid(X, Y, Z); }

	/** Places Kind against the face (Nx, Ny, Nz) of the block at (Ax, Ay, Az). */
	bool Place(int32 Ax, int32 Ay, int32 Az, int32 Nx, int32 Ny, int32 Nz, FName Kind, const FString& By, const FString& On, const TArray<FCubeHitbox>& Players, FCubeWorldUpdate& Out);
	bool Break(int32 X, int32 Y, int32 Z, const FString& By, const FString& On, FCubeWorldUpdate& Out, FBlockDef& OutBroken);

	/** A change another server wrote. Returns whether it changed anything here. */
	bool Apply(const FIntVector& At, FName Kind, const FString& By, const FString& On, int64 When, UWorldCube* Row);
	/** A change of this server's own that the platform has answered: remember the row. */
	void Remember(const FIntVector& At, UWorldCube* Row);
	/** A write of this block landed: it is in the table from now on. */
	void Touch(const FIntVector& At);
	/** A row that is gone (the world was reset, or a region cleared): the block is the generated one again. Returns whether there was one. */
	bool Forget(const FIntVector& At);
	/** The blocks held here that a read of the table did not find, leaving out any set, heard or written after Version AsOf. */
	TArray<FIntVector> Missing(const TSet<FIntVector>& Found, uint64 AsOf) const;
	/** A row's key, x:y:z, as a block; false when it is not one. */
	static bool ParseKey(const FString& Key, FIntVector& Out);

	/**
	 * An explosion as Minecraft's: rays go out from the centre towards every point of a 16 × 16 × 16 cube's surface,
	 * each with an intensity of power × (0.7 to 1.3). Every 0.3 blocks a ray loses 0.225 and, in a block,
	 * (blast resistance + 0.3) × 0.3; a block the ray still has intensity for is destroyed. Nothing drops.
	 */
	void Explode(double Cx, double Cy, double Cz, double Power, int32 Seed, const FString& By, const FString& On, int32 OnlyRegion, FCubeWorldUpdate& Out);

	/**
	 * What an explosion does to a player: within twice the power, impact is (1 − distance / (2 × power)) × the share
	 * of the hitbox the centre can see; damage is ⌊(impact² + impact) / 2 × 7 × 2 × power + 1⌋ and the player is
	 * thrown away from the centre with the impact. False when out of range.
	 */
	bool Blast(double Cx, double Cy, double Cz, double Power, const FCubeHitbox& P, double Eye, double& OutDamage, double& OutNx, double& OutNy, double& OutImpact) const;
	/** The share of points spread through the hitbox from which the centre is in plain sight. */
	double Exposure(double Cx, double Cy, double Cz, const FCubeHitbox& P) const;

	static bool Intersects(const FCubeHitbox& P, int32 X, int32 Y, int32 Z);
	/** Distance from a point to the nearest point of the block at (X, Y, Z). */
	static double DistanceToBlock(double Px, double Py, double Pz, int32 X, int32 Y, int32 Z);
	static double DistanceToHitbox(double Px, double Py, double Pz, const FCubeHitbox& H);

private:
	bool Clear(double X, double Y, double Z, double Tx, double Ty, double Tz) const;
	void Set(int32 X, int32 Y, int32 Z, FName Kind, const FString& By, const FString& On, FCubeWorldUpdate& Out);
	/** Lets the column above (X, Y, Z) fall: every gravity block from the first unsupported one up. */
	void Settle(int32 X, int32 Y, int32 Z, FCubeWorldUpdate& Out);
};

/** The fall a player is in, as their moves report it. */
struct FCubePlayerFall
{
	bool bAirborne = false;
	double Peak = 0;

	/**
	 * One move. In the air, the peak rises with it; on landing, the damage: ceil(peak - z - 3), never below 0, and the
	 * fall is over. SaidPeak is the client's own highest point since the ground (FCubeBody::Peak), which carries a fall
	 * over a border: the part of it that happened on the old server counts too. It never makes a fall shorter than the
	 * moves themselves reached.
	 */
	double Step(double Z, bool bOnGround, TOptional<double> SaidPeak = TOptional<double>());
};

/** A player another server saw is the one walking in over the border when it saw them in the last 5 s, alive (World.Arriving on the C# side). */
inline bool CubeSeenJustNow(int64 SeenAt, double Health, int64 Now) { return Now - SeenAt < 5000 && Health > 0; }

/** How far outside a region the last server may have seen a player who walked in over its border, in blocks. */
constexpr double CubeCrossingBand = 4.0;

/**
 * Whether a player another server just saw at (X, Y) walked into Region over its border: they were seen in it or within
 * CubeCrossingBand of it. Then X and Y are moved inside the region, where they stand. Seen farther away, they jumped to
 * this room from the server list, and start at its spawn. World.Arriving on the C# side.
 */
inline bool CubeCrossedInto(int32 Region, double& X, double& Y)
{
	int32 X0, X1, Y0, Y1;
	CubeSpec::RegionBounds(Region, X0, X1, Y0, Y1);
	if (X < X0 - CubeCrossingBand || X > X1 + CubeCrossingBand || Y < Y0 - CubeCrossingBand || Y > Y1 + CubeCrossingBand) return false;
	X = FMath::Clamp(X, X0 + CubeSpec::Width / 2, X1 - CubeSpec::Width / 2);
	Y = FMath::Clamp(Y, Y0 + CubeSpec::Width / 2, Y1 - CubeSpec::Width / 2);
	return true;
}

/** What a player carries, per block kind. */
struct FCubeInventory
{
	TMap<FName, int32> Stacks;

	static FCubeInventory Starting();
	static FCubeInventory Parse(const FString& StacksJson);
	FString ToJson() const;
	int32 Total() const;
	int32 Count(FName Kind) const { const int32* C = Stacks.Find(Kind); return C ? *C : 0; }
	bool Take(FName Kind);
	/** Adds one item; a full stack (64) takes no more, as in Minecraft. */
	bool Give(FName Kind);
	/** The same count of every kind (a kind not listed counts 0). */
	bool Same(const FCubeInventory& Other) const;
};

/**
 * One player's inventory row as this server knows it, for merging the other writers' rows into the inventory it holds:
 * the old server's last write after a crossing, the refill function's top-up. A row heard is either this server's own
 * write coming back (one of the rows it wrote and has not heard yet) or someone else's: then what that writer changed
 * since the row this server last knew is added to what the player did here, kind by kind. InventorySync on the C# side.
 */
struct FCubeInventorySync
{
	/** A write of ours not heard back after this long is taken to be in the row (an uplink that sends a server none of its own). */
	static constexpr int64 EchoMs = 3000;

	/** The row as this server last knew it: read at the join, or heard since. */
	FCubeInventory Base;
	/** This server's writes not heard back yet, oldest first, and when each went out. */
	TArray<TPair<FCubeInventory, int64>> Written;

	void Wrote(const FCubeInventory& Stacks, int64 Now);
	/** A row was heard. False when it is this server's own write (nothing changes); else Merged is the inventory to hold now. */
	bool Heard(const FCubeInventory& Ours, const FCubeInventory& Theirs, int64 Now, FCubeInventory& Merged);
	/** Ours plus what Theirs changed since Base, each kind kept within 0 and a stack. */
	static FCubeInventory Merge(const FCubeInventory& Ours, const FCubeInventory& InBase, const FCubeInventory& Theirs);
};
