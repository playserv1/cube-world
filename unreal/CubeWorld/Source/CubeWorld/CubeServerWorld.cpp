#include "CubeServerWorld.h"
#include "CubeEntities.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Policies/CondensedJsonPrintPolicy.h"

bool FCubeServerWorld::Place(int32 Ax, int32 Ay, int32 Az, int32 Nx, int32 Ny, int32 Nz, FName Kind, const FString& By, const FString& On, const TArray<FCubeHitbox>& Players, FCubeWorldUpdate& Out)
{
	if (!Voxels.Block(Kind).IsPlaceable() || !IsSolid(Ax, Ay, Az) || FMath::Abs(Nx) + FMath::Abs(Ny) + FMath::Abs(Nz) != 1) return false;
	const int32 X = Ax + Nx, Y = Ay + Ny, Z = Az + Nz;
	if (!Inside(X, Y, Z) || BlockAt(X, Y, Z).IsSolid()) return false;
	for (const FCubeHitbox& P : Players) if (Intersects(P, X, Y, Z)) return false;
	Set(X, Y, Z, Kind, By, On, Out);
	Settle(X, Y, Z, Out);
	return true;
}

bool FCubeServerWorld::Break(int32 X, int32 Y, int32 Z, const FString& By, const FString& On, FCubeWorldUpdate& Out, FBlockDef& OutBroken)
{
	const FBlockDef& Block = BlockAt(X, Y, Z);
	if (!Inside(X, Y, Z) || !Block.IsSolid() || !Block.IsBreakable()) return false;
	OutBroken = Block;
	Set(X, Y, Z, TEXT("air"), By, On, Out);
	Settle(X, Y, Z, Out);
	return true;
}

bool FCubeServerWorld::Apply(const FIntVector& At, FName Kind, const FString& By, const FString& On, int64 When, UWorldCube* Row)
{
	if (!Inside(At.X, At.Y, At.Z)) return false;
	FCubeOverride* Known = Overrides.Find(At);
	if (Known && Known->Kind == Kind)
	{
		// The same block, seen again: only take the row so the next write of it is an update, not a duplicate.
		if (Row && !Known->Row.IsValid()) Known->Row.Reset(Row);
		if (When > Known->At) Known->At = When;
		Known->Version = ++Version;
		return false;
	}
	FCubeOverride& O = Overrides.FindOrAdd(At);
	O.Kind = Kind; O.By = By; O.On = On; O.At = When; O.Version = ++Version;
	if (Row) O.Row.Reset(Row);
	Voxels.Set(At.X, At.Y, At.Z, Kind);
	return true;
}

void FCubeServerWorld::Remember(const FIntVector& At, UWorldCube* Row)
{
	if (FCubeOverride* O = Overrides.Find(At)) O->Row.Reset(Row);
}

void FCubeServerWorld::Touch(const FIntVector& At)
{
	if (FCubeOverride* O = Overrides.Find(At)) O->Version = ++Version;
}

bool FCubeServerWorld::Forget(const FIntVector& At)
{
	if (Overrides.Remove(At) == 0) return false;
	Voxels.Set(At.X, At.Y, At.Z, NAME_None);
	++Version;
	return true;
}

TArray<FIntVector> FCubeServerWorld::Missing(const TSet<FIntVector>& Found, uint64 AsOf) const
{
	TArray<FIntVector> Out;
	for (const auto& Pair : Overrides) if (Pair.Value.Version <= AsOf && !Found.Contains(Pair.Key)) Out.Add(Pair.Key);
	return Out;
}

bool FCubeServerWorld::ParseKey(const FString& Key, FIntVector& Out)
{
	TArray<FString> Parts;
	if (Key.ParseIntoArray(Parts, TEXT(":"), false) != 3) return false;
	for (const FString& Part : Parts) if (Part.IsEmpty() || !Part.IsNumeric()) return false;
	Out = FIntVector(FCString::Atoi(*Parts[0]), FCString::Atoi(*Parts[1]), FCString::Atoi(*Parts[2]));
	return true;
}

void FCubeServerWorld::Explode(double Cx, double Cy, double Cz, double Power, int32 Seed, const FString& By, const FString& On, int32 OnlyRegion, FCubeWorldUpdate& Out)
{
	// The same seed on every server: the rays, and so the crater, come out the same wherever they are worked out.
	FDotNetRandom Random(Seed);
	TSet<FIntVector> Destroyed;
	for (int32 I = 0; I < 16; I++)
		for (int32 J = 0; J < 16; J++)
			for (int32 K = 0; K < 16; K++)
			{
				if (I != 0 && I != 15 && J != 0 && J != 15 && K != 0 && K != 15) continue;
				double Dx = I / 15.0 * 2 - 1, Dy = J / 15.0 * 2 - 1, Dz = K / 15.0 * 2 - 1;
				const double Length = FMath::Sqrt(Dx * Dx + Dy * Dy + Dz * Dz);
				Dx /= Length; Dy /= Length; Dz /= Length;
				double X = Cx, Y = Cy, Z = Cz;
				for (double Intensity = Power * (0.7 + Random.NextDouble() * 0.6); Intensity > 0; Intensity -= 0.22500001)
				{
					const int32 Bx = FMath::FloorToInt32(X), By2 = FMath::FloorToInt32(Y), Bz = FMath::FloorToInt32(Z);
					if (Inside(Bx, By2, Bz))
					{
						const FBlockDef& Block = BlockAt(Bx, By2, Bz);
						if (Block.IsSolid())
						{
							Intensity -= (Block.BlastResistance + 0.3) * 0.3;
							if (Intensity > 0 && Block.IsBreakable()) Destroyed.Add(FIntVector(Bx, By2, Bz));
						}
					}
					X += Dx * 0.3; Y += Dy * 0.3; Z += Dz * 0.3;
				}
			}

	TArray<FIntVector> Ordered;
	for (const FIntVector& C : Destroyed) if (CubeSpec::RegionOf(C.X, C.Y) == OnlyRegion) Ordered.Add(C);
	Ordered.Sort([](const FIntVector& A, const FIntVector& B) { return A.Z < B.Z; });
	for (const FIntVector& C : Ordered) Set(C.X, C.Y, C.Z, TEXT("air"), By, On, Out);
	// Each broken block of a column settles what stands on it: a blast can break a column in more than one place.
	for (const FIntVector& C : Ordered) Settle(C.X, C.Y, C.Z, Out);
}

bool FCubeServerWorld::Blast(double Cx, double Cy, double Cz, double Power, const FCubeHitbox& P, double Eye, double& OutDamage, double& OutNx, double& OutNy, double& OutImpact) const
{
	const double Fx = P.X - Cx, Fy = P.Y - Cy, Fz = P.Z - Cz;
	const double Distance = FMath::Sqrt(Fx * Fx + Fy * Fy + Fz * Fz) / CubeSpec::BlastReach;
	if (Distance > 1) return false;
	double Dx = P.X - Cx, Dy = P.Y - Cy;
	const double Dz = P.Z + Eye - Cz;
	double Length = FMath::Sqrt(Dx * Dx + Dy * Dy + Dz * Dz);
	if (Length < 1e-9) { Dx = 0; Dy = 0; Length = 1; }
	const double Impact = (1 - Distance) * Exposure(Cx, Cy, Cz, P);
	OutDamage = FMath::Floor((Impact * Impact + Impact) / 2 * 7 * (2 * Power) + 1);
	OutNx = Dx / Length; OutNy = Dy / Length; OutImpact = Impact;
	return true;
}

double FCubeServerWorld::Exposure(double Cx, double Cy, double Cz, const FCubeHitbox& P) const
{
	const double Half = CubeSpec::Width / 2;
	const double Sx = 1 / (CubeSpec::Width * 2 + 1), Sz = 1 / (P.Height * 2 + 1);
	const double Offset = (1 - FMath::Floor(1 / Sx) * Sx) / 2;
	int32 Seen = 0, All = 0;
	for (double A = 0.0; A <= 1; A += Sx)
		for (double B = 0.0; B <= 1; B += Sx)
			for (double C = 0.0; C <= 1; C += Sz)
			{
				All++;
				const double X = P.X - Half + A * CubeSpec::Width + Offset, Y = P.Y - Half + B * CubeSpec::Width + Offset, Z = P.Z + C * P.Height;
				if (Clear(X, Y, Z, Cx, Cy, Cz)) Seen++;
			}
	return All == 0 ? 0 : Seen / (double)All;
}

bool FCubeServerWorld::Clear(double X, double Y, double Z, double Tx, double Ty, double Tz) const
{
	const double Dx = Tx - X, Dy = Ty - Y, Dz = Tz - Z;
	const int32 Steps = FMath::CeilToInt32(FMath::Sqrt(Dx * Dx + Dy * Dy + Dz * Dz) / 0.1);
	for (int32 I = 0; I < Steps; I++)
	{
		const double T = I / (double)Steps;
		if (IsSolid(FMath::FloorToInt32(X + Dx * T), FMath::FloorToInt32(Y + Dy * T), FMath::FloorToInt32(Z + Dz * T))) return false;
	}
	return true;
}

bool FCubeServerWorld::Intersects(const FCubeHitbox& P, int32 X, int32 Y, int32 Z)
{
	const double Half = CubeSpec::Width / 2;
	return P.X - Half < X + 1 && P.X + Half > X && P.Y - Half < Y + 1 && P.Y + Half > Y && P.Z < Z + 1 && P.Z + P.Height > Z;
}

double FCubeServerWorld::DistanceToBlock(double Px, double Py, double Pz, int32 X, int32 Y, int32 Z)
{
	const double Dx = FMath::Max(0.0, FMath::Max(X - Px, Px - (X + 1)));
	const double Dy = FMath::Max(0.0, FMath::Max(Y - Py, Py - (Y + 1)));
	const double Dz = FMath::Max(0.0, FMath::Max(Z - Pz, Pz - (Z + 1)));
	return FMath::Sqrt(Dx * Dx + Dy * Dy + Dz * Dz);
}

double FCubeServerWorld::DistanceToHitbox(double Px, double Py, double Pz, const FCubeHitbox& H)
{
	const double Half = CubeSpec::Width / 2;
	const double Dx = FMath::Max(0.0, FMath::Max(H.X - Half - Px, Px - (H.X + Half)));
	const double Dy = FMath::Max(0.0, FMath::Max(H.Y - Half - Py, Py - (H.Y + Half)));
	const double Dz = FMath::Max(0.0, FMath::Max(H.Z - Pz, Pz - (H.Z + H.Height)));
	return FMath::Sqrt(Dx * Dx + Dy * Dy + Dz * Dz);
}

// Every change is an upsert, even "air" where the terrain is air, as the C# servers write it: the other Unreal
// servers' windows on `at` see upserts alone, and a delete reaches a server only over its uplink subscription. So a
// broken block is written as air.
void FCubeServerWorld::Set(int32 X, int32 Y, int32 Z, FName Kind, const FString& By, const FString& On, FCubeWorldUpdate& Out)
{
	const FIntVector At(X, Y, Z);
	FCubeOverride& O = Overrides.FindOrAdd(At);
	O.Kind = Kind; O.By = By; O.On = On; O.Version = ++Version;
	Voxels.Set(X, Y, Z, Kind);
	Out.Changes.Add({ At, Kind, By, On });
}

void FCubeServerWorld::Settle(int32 X, int32 Y, int32 Z, FCubeWorldUpdate& Out)
{
	int32 Zz = BlockAt(X, Y, Z).bGravity ? Z : Z + 1;
	for (; Zz < CubeSpec::MaxZ && BlockAt(X, Y, Zz).bGravity; Zz++)
	{
		int32 To = Zz;
		while (To - 1 >= CubeSpec::MinZ && !IsSolid(X, Y, To - 1)) To--;
		if (To == Zz) continue;

		const FCubeOverride* Cube = Overrides.Find(FIntVector(X, Y, Zz));
		const FName Kind = BlockAt(X, Y, Zz).Kind;
		const FString By = Cube ? Cube->By : FString(), On = Cube ? Cube->On : FString();
		Set(X, Y, Zz, TEXT("air"), By, On, Out);
		Set(X, Y, To, Kind, By, On, Out);
		Out.Falls.Add({ Kind, X, Y, Zz, To });
	}
}

// ── the inventory ────────────────────────────────────────────────────────────────────────────────

double FCubePlayerFall::Step(double Z, bool bOnGround, TOptional<double> SaidPeak)
{
	if (bOnGround)
	{
		const double Damage = bAirborne ? FMath::Max(0.0, FMath::CeilToDouble(Peak - Z - CubeSpec::SafeFallDistance)) : 0.0;
		bAirborne = false;
		return Damage;
	}
	Peak = bAirborne ? FMath::Max(Peak, Z) : Z;
	if (SaidPeak.IsSet()) Peak = FMath::Max(Peak, FMath::Min(SaidPeak.GetValue(), CubeSpec::MaxZ + 8.0));
	bAirborne = true;
	return 0;
}

FCubeInventory FCubeInventory::Starting()
{
	FCubeInventory I;
	for (const FName& Kind : CubeSpec::Hotbar()) I.Stacks.Add(Kind, CubeSpec::StartingStack);
	return I;
}

FCubeInventory FCubeInventory::Parse(const FString& StacksJson)
{
	TSharedPtr<FJsonObject> Object;
	if (StacksJson.IsEmpty() || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(StacksJson), Object) || !Object.IsValid()) return Starting();
	FCubeInventory I;
	for (const auto& Pair : Object->Values) I.Stacks.Add(FName(*Pair.Key), (int32)Pair.Value->AsNumber());
	return I;
}

FString FCubeInventory::ToJson() const
{
	const TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	for (const auto& Pair : Stacks) Object->SetNumberField(Pair.Key.ToString(), Pair.Value);
	FString Out;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
	FJsonSerializer::Serialize(Object, Writer);
	return Out;
}

int32 FCubeInventory::Total() const
{
	int32 Sum = 0;
	for (const auto& Pair : Stacks) Sum += Pair.Value;
	return Sum;
}

bool FCubeInventory::Take(FName Kind)
{
	if (Count(Kind) <= 0) return false;
	Stacks[Kind]--;
	return true;
}

bool FCubeInventory::Give(FName Kind)
{
	if (!CubeSpec::Block(Kind).IsPlaceable() || Count(Kind) >= CubeSpec::StackSize) return false;
	Stacks.FindOrAdd(Kind) = Count(Kind) + 1;
	return true;
}

bool FCubeInventory::Same(const FCubeInventory& Other) const
{
	for (const auto& Pair : Stacks) if (Other.Count(Pair.Key) != Pair.Value) return false;
	for (const auto& Pair : Other.Stacks) if (Count(Pair.Key) != Pair.Value) return false;
	return true;
}

void FCubeInventorySync::Wrote(const FCubeInventory& Stacks, int64 Now)
{
	Written.Add({ Stacks, Now });
	if (Written.Num() > 32) Written.RemoveAt(0);
}

bool FCubeInventorySync::Heard(const FCubeInventory& Ours, const FCubeInventory& Theirs, int64 Now, FCubeInventory& Merged)
{
	// The platform keeps one write of a row at a time and sends the latest: a later write of ours heard means the
	// earlier ones are behind us too.
	const int32 Own = Written.IndexOfByPredicate([&Theirs](const TPair<FCubeInventory, int64>& W) { return W.Key.Same(Theirs); });
	if (Own != INDEX_NONE)
	{
		Written.RemoveAt(0, Own + 1);
		Base = Theirs;
		return false;
	}
	// Writes of ours that never came back are in the row: another writer's change counts from the latest of them.
	const int32 Landed = Written.FindLastByPredicate([Now](const TPair<FCubeInventory, int64>& W) { return Now - W.Value >= EchoMs; });
	if (Landed != INDEX_NONE)
	{
		Base = Written[Landed].Key;
		Written.RemoveAt(0, Landed + 1);
	}
	Merged = Merge(Ours, Base, Theirs);
	Base = Theirs;
	return true;
}

FCubeInventory FCubeInventorySync::Merge(const FCubeInventory& Ours, const FCubeInventory& InBase, const FCubeInventory& Theirs)
{
	TSet<FName> Kinds;
	for (const auto& Pair : Ours.Stacks) Kinds.Add(Pair.Key);
	for (const auto& Pair : InBase.Stacks) Kinds.Add(Pair.Key);
	for (const auto& Pair : Theirs.Stacks) Kinds.Add(Pair.Key);
	FCubeInventory Out;
	for (const FName& Kind : Kinds) Out.Stacks.Add(Kind, FMath::Clamp(Ours.Count(Kind) + Theirs.Count(Kind) - InBase.Count(Kind), 0, CubeSpec::StackSize));
	return Out;
}
