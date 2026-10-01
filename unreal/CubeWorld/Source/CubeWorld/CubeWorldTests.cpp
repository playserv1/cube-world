// The world's rules for a block whose row is deleted, checked without a platform: on the server it goes back to the
// generated terrain, the wire names that with its own kind, and a read of the table takes for gone only what it did
// not find and what did not change after it began.
#include "CubeServerWorld.h"
#include "CubeSpec.h"
#include "CubeVoxelWorld.h"
#include "CubeWorldGameInstance.h"
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCubeWorldGeneratedKindTest,
	"CubeWorld.Wire.GeneratedKindRestoresTheTerrain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCubeWorldGeneratedKindTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("None goes on the wire as the generated index"), (int32)CubeSpec::KindIndex(NAME_None), (int32)CubeSpec::GeneratedIndex);
	TestTrue(TEXT("and comes back as None"), CubeSpec::KindOf(CubeSpec::GeneratedIndex) == NAME_None);
	TestTrue(TEXT("no block kind has the generated index"), CubeSpec::Blocks().Num() < CubeSpec::GeneratedIndex);
	for (const FBlockDef& B : CubeSpec::Blocks())
		TestTrue(FString::Printf(TEXT("%s comes back as itself"), *B.Kind.ToString()), CubeSpec::KindOf(CubeSpec::KindIndex(B.Kind)) == B.Kind);
	TestTrue(TEXT("an unknown index is still air"), CubeSpec::KindOf(200) == TEXT("air"));

	// What a client does with a change: an "air" override keeps the grass dug out, the generated kind brings it back.
	FCubeVoxelWorld Voxels;
	Voxels.Set(3, 3, -1, CubeSpec::KindOf(CubeSpec::KindIndex(TEXT("air"))));
	TestTrue(TEXT("a dug grass block is air"), Voxels.KindAt(3, 3, -1) == TEXT("air"));
	Voxels.Set(3, 3, -1, CubeSpec::KindOf(CubeSpec::GeneratedIndex));
	TestTrue(TEXT("the generated kind brings the grass back"), Voxels.KindAt(3, 3, -1) == TEXT("grass"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCubeWorldForgetTest,
	"CubeWorld.Server.DeletedRowsGoBackToTheTerrain",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCubeWorldForgetTest::RunTest(const FString& Parameters)
{
	FCubeServerWorld World;
	FCubeWorldUpdate Update;
	FBlockDef Broken;
	TestTrue(TEXT("a grass block breaks"), World.Break(3, 3, -1, TEXT("plr_a"), TEXT("alpha"), Update, Broken));
	TestTrue(TEXT("into air"), World.BlockAt(3, 3, -1).Kind == TEXT("air"));
	TestTrue(TEXT("forgetting its row brings it back"), World.Forget(FIntVector(3, 3, -1)));
	TestTrue(TEXT("as the terrain's grass"), World.BlockAt(3, 3, -1).Kind == TEXT("grass") && !World.Overrides.Contains(FIntVector(3, 3, -1)));
	TestFalse(TEXT("a block with no row has nothing to forget"), World.Forget(FIntVector(3, 3, -1)));

	const FIntPoint Oak = CubeTreeSpots()[0];
	TestTrue(TEXT("an oak's log is felled"), World.Break(Oak.X, Oak.Y, 0, TEXT("plr_a"), TEXT("alpha"), Update, Broken));
	World.Forget(FIntVector(Oak.X, Oak.Y, 0));
	TestTrue(TEXT("and stands again when its row goes"), World.BlockAt(Oak.X, Oak.Y, 0).Kind == TEXT("wood"));

	// A read of the table that began at AsOf found only the gold: the brick is gone, the glass came after the read.
	World.Apply(FIntVector(1, 1, 0), TEXT("brick"), TEXT("plr_a"), TEXT("alpha"), 1, nullptr);
	World.Apply(FIntVector(2, 1, 0), TEXT("gold"), TEXT("plr_a"), TEXT("alpha"), 1, nullptr);
	const uint64 AsOf = World.Version;
	World.Apply(FIntVector(4, 1, 0), TEXT("glass"), TEXT("plr_b"), TEXT("beta"), 2, nullptr);
	const TSet<FIntVector> Found = { FIntVector(2, 1, 0) };
	const TArray<FIntVector> Missing = World.Missing(Found, AsOf);
	TestTrue(TEXT("what the read did not find is missing"), Missing.Num() == 1 && Missing.Contains(FIntVector(1, 1, 0)));
	World.Touch(FIntVector(1, 1, 0));
	TestFalse(TEXT("unless a write of it landed after the read began"), World.Missing(Found, AsOf).Contains(FIntVector(1, 1, 0)));
	World.Apply(FIntVector(2, 1, 0), TEXT("gold"), TEXT("plr_a"), TEXT("alpha"), 3, nullptr);
	TestTrue(TEXT("a block heard again as it was counts as heard"), World.Overrides[FIntVector(2, 1, 0)].Version > AsOf);

	FIntVector At;
	TestTrue(TEXT("a row's key is its block"), FCubeServerWorld::ParseKey(TEXT("5:6:-1"), At) && At == FIntVector(5, 6, -1));
	TestFalse(TEXT("a record id is not a block"), FCubeServerWorld::ParseKey(TEXT("rec_01J8ZKQ4X0"), At));
	TestFalse(TEXT("nor are two numbers"), FCubeServerWorld::ParseKey(TEXT("5:6"), At));
	TestFalse(TEXT("nor an empty part"), FCubeServerWorld::ParseKey(TEXT("5::6"), At));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCubeWorldCrossingWelcomeTest,
	"CubeWorld.Crossing.WelcomeOfACrossingKeepsTheWorld",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// A fresh join draws the world again from the welcome; a crossing keeps the world on screen and applies only what the
// next server's snapshot changed (rebuilding every chunk at every border cost a frame of 15 ms).
bool FCubeWorldCrossingWelcomeTest::RunTest(const FString& Parameters)
{
	UCubeWorldGameInstance* Game = NewObject<UCubeWorldGameInstance>();
	TOptional<bool> Teleport;
	int32 Changed = 0;
	Game->OnWelcome.AddLambda([&Teleport](const FCubePose&, bool bTeleport) { Teleport = bTeleport; });
	Game->OnCubes.AddLambda([&Changed](const TArray<FIntVector>& Cells) { Changed += Cells.Num(); });
	const uint8 Stone = CubeSpec::KindIndex(TEXT("stone")), Dirt = CubeSpec::KindIndex(TEXT("dirt"));

	Game->OnWelcomed(TEXT("alpha"), TEXT("yellow"), TEXT("yellow-alpha"), 3, FCubePose{ 12, 36, 0, 20 }, {}, 1);
	Game->OnWorldChunk({ { 1, 2, 3, Stone } }, true);
	TestTrue(TEXT("a fresh join draws the world again"), Teleport.IsSet() && Teleport.GetValue());
	TestTrue(TEXT("with the welcome's blocks"), Game->World.KindAt(1, 2, 3) == TEXT("stone"));

	Teleport.Reset();
	Changed = 0;
	Game->Crossing.bSet = true;
	Game->OnWelcomed(TEXT("beta"), TEXT("purple"), TEXT("purple-beta"), 4, FCubePose{ 24.5, 36, 0, 20 }, {}, 1);
	Game->OnWorldChunk({ { 1, 2, 3, Stone }, { 4, 5, 6, Dirt } }, true);
	TestTrue(TEXT("a crossing keeps the world"), Teleport.IsSet() && !Teleport.GetValue());
	TestEqual(TEXT("and changes only the block the next server holds otherwise"), Changed, 1);
	TestTrue(TEXT("which is there"), Game->World.KindAt(4, 5, 6) == TEXT("dirt") && Game->World.KindAt(1, 2, 3) == TEXT("stone"));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
