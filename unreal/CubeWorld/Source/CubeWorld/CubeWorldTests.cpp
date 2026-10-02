// The world's rules for a block whose row is deleted, checked without a platform: on the server it goes back to the
// generated terrain, the wire names that with its own kind, and a read of the table takes for gone only what it did
// not find and what did not change after it began.
#include "CubePhysics.h"
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCubeWorldCrossingFallTest,
	"CubeWorld.Crossing.AFallGoesOnOverTheBorder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// The fall rule both servers apply (World.cs PlayerFall on the C# side), and the peak the client's body keeps for it.
bool FCubeWorldCrossingFallTest::RunTest(const FString& Parameters)
{
	FCubePlayerFall Fall;
	TestEqual(TEXT("in the air, no damage yet"), Fall.Step(10, false) + Fall.Step(4, false), 0.0);
	TestEqual(TEXT("landing from 10 hurts by 7"), Fall.Step(0, true), 7.0);
	TestFalse(TEXT("and the fall is over"), Fall.bAirborne);
	Fall.Step(2.5, false);
	TestEqual(TEXT("a drop of 2.5 does not hurt"), Fall.Step(0, true), 0.0);

	FCubePlayerFall Crossed;
	Crossed.Step(5, false, TOptional<double>(10.0));
	TestEqual(TEXT("a fall that began on the old server hurts on the next one"), Crossed.Step(0, true), 7.0);
	FCubePlayerFall Lied;
	Lied.Step(10, false);
	Lied.Step(5, false, TOptional<double>(1.0));
	TestEqual(TEXT("a client cannot make a fall shorter by saying so"), Lied.Step(0, true), 7.0);
	FCubePlayerFall Standing;
	TestEqual(TEXT("standing on the ground is no fall"), Standing.Step(0, true, TOptional<double>(30.0)), 0.0);

	// The body: off a ten-block tower, the peak is the top all the way down, and the ground again once it lands.
	const FCubeSolidQuery Flat = [](int32, int32, int32 Z) { return Z < 0; };
	FCubeBody Body;
	Body.Teleport(5, 5, 10);
	FCubeInput Idle;
	for (int32 I = 0; I < 10; I++) CubePhysics::Tick(Body, Idle, Flat);
	TestTrue(TEXT("falling, the peak is the tower's top"), Body.Z < 10 && Body.Peak == 10);
	for (int32 I = 0; I < 60; I++) CubePhysics::Tick(Body, Idle, Flat);
	TestTrue(TEXT("landed, the peak is the ground"), Body.bOnGround && Body.Peak == Body.Z);

	// Who walks in over a border: seen by another server in the last 5 s, alive.
	TestTrue(TEXT("seen 200 ms ago"), CubeSeenJustNow(1000000 - 200, 13, 1000000));
	TestFalse(TEXT("not seen for 5 s"), CubeSeenJustNow(1000000 - 5000, 13, 1000000));
	TestFalse(TEXT("dead"), CubeSeenJustNow(1000000 - 200, 0, 1000000));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCubeWorldInventorySyncTest,
	"CubeWorld.Crossing.InventoryWritersMerge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// Two writers of one player's inventory row across a crossing, and the refill function: nothing undone, nothing lost.
// The same cases as InventorySyncTests.cs on the C# side.
bool FCubeWorldInventorySyncTest::RunTest(const FString& Parameters)
{
	auto Stacks = [](int32 Dirt, int32 Stone = 10) { FCubeInventory I; I.Stacks.Add(TEXT("dirt"), Dirt); I.Stacks.Add(TEXT("stone"), Stone); return I; };
	FCubeInventory Merged;

	FCubeInventorySync Late;
	Late.Base = Stacks(10);
	TestTrue(TEXT("the old server's late write is another writer's"), Late.Heard(Stacks(10), Stacks(9), 0, Merged));
	TestEqual(TEXT("a block spent there just before the crossing stays spent"), Merged.Count(TEXT("dirt")), 9);

	FCubeInventorySync Both;
	Both.Base = Stacks(10);
	Both.Heard(Stacks(8), Stacks(9), 0, Merged);
	TestEqual(TEXT("and what the player did here meanwhile is kept too"), Merged.Count(TEXT("dirt")), 7);

	FCubeInventorySync Own;
	Own.Base = Stacks(10);
	Own.Wrote(Stacks(9), 0);
	Own.Wrote(Stacks(8), 0);
	TestFalse(TEXT("this server's own latest write coming back changes nothing"), Own.Heard(Stacks(8), Stacks(8), 0, Merged));
	TestTrue(TEXT("its base is that write"), Own.Base.Same(Stacks(8)) && Own.Written.Num() == 0);
	TestTrue(TEXT("an earlier one heard after it is someone else's"), Own.Heard(Stacks(8), Stacks(9), 0, Merged));

	FCubeInventorySync Refill;
	Refill.Base = Stacks(10);
	Refill.Wrote(Stacks(9), 0);
	Refill.Heard(Stacks(9), Stacks(9), 0, Merged);
	Refill.Wrote(Stacks(8), 0);
	Refill.Heard(Stacks(8), Stacks(10, 11), 0, Merged);
	TestTrue(TEXT("a refill tops up what the player holds now"), Merged.Count(TEXT("dirt")) == 9 && Merged.Count(TEXT("stone")) == 11);

	FCubeInventorySync Unheard;
	Unheard.Base = Stacks(10);
	Unheard.Wrote(Stacks(8), 0);
	Unheard.Heard(Stacks(8), Stacks(9, 11), 5000, Merged);
	TestTrue(TEXT("a write of ours never heard back counts as in the row"), Merged.Count(TEXT("dirt")) == 9 && Merged.Count(TEXT("stone")) == 11);

	const FCubeInventory Clamped = FCubeInventorySync::Merge(Stacks(64, 0), Stacks(10, 5), Stacks(20, 1));
	TestTrue(TEXT("a merge keeps every kind within 0 and a stack"), Clamped.Count(TEXT("dirt")) == CubeSpec::StackSize && Clamped.Count(TEXT("stone")) == 0);
	FCubeInventory OnlyDirt, OnlyGold;
	OnlyDirt.Stacks.Add(TEXT("dirt"), 3);
	OnlyGold.Stacks.Add(TEXT("gold"), 2);
	const FCubeInventory Kinds = FCubeInventorySync::Merge(OnlyDirt, FCubeInventory(), OnlyGold);
	TestTrue(TEXT("a kind only one side lists counts as none on the other"), Kinds.Count(TEXT("dirt")) == 3 && Kinds.Count(TEXT("gold")) == 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCubeWorldKindNamesTest,
	"CubeWorld.Wire.KindsGoOutInTheRegistrysSpelling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// An FName keeps the casing of its first spelling in the process, and the engine names "Stone" before the registry runs:
// what goes out to the tables and to the JSON clients is the registry's own lowercase name, whatever the FName says.
bool FCubeWorldKindNamesTest::RunTest(const FString& Parameters)
{
	for (const FBlockDef& B : CubeSpec::Blocks())
	{
		TestEqual(FString::Printf(TEXT("%s goes out as its registry name"), *B.Name), CubeSpec::KindName(B.Kind), B.Name);
		TestEqual(FString::Printf(TEXT("%s is lowercase"), *B.Name), B.Name, B.Name.ToLower());
	}
	TestEqual(TEXT("an FName spelled STONE goes out as stone"), CubeSpec::KindName(FName(TEXT("STONE"))), FString(TEXT("stone")));
	TestEqual(TEXT("so does Stone"), CubeSpec::KindName(FName(TEXT("Stone"))), FString(TEXT("stone")));
	const FString Row = FCubeInventory::Starting().ToJson();
	TestTrue(TEXT("an inventory row goes out with stone"), Row.Contains(TEXT("\"stone\":64"), ESearchCase::CaseSensitive));
	TestFalse(TEXT("and without Stone"), Row.Contains(TEXT("\"Stone\""), ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCubeWorldInventoryCaseTest,
	"CubeWorld.Inventory.OneKindUnderTwoSpellingsAddsUp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// Unreal servers wrote "Stone" until 2026-10-02, and the refill function topped such rows up as "stone": a row lists one
// kind twice, and both counts are the player's, within a stack. Taking the last key left a player 1 stone of 64.
bool FCubeWorldInventoryCaseTest::RunTest(const FString& Parameters)
{
	const FCubeInventory Topped = FCubeInventory::Parse(TEXT("{\"grass\":64,\"Stone\":30,\"stone\":3}"));
	TestEqual(TEXT("the server adds the two keys up"), Topped.Count(TEXT("stone")), 33);
	TestEqual(TEXT("within a stack"), FCubeInventory::Parse(TEXT("{\"Stone\":64,\"stone\":1}")).Count(TEXT("stone")), CubeSpec::StackSize);
	const FString Row = Topped.ToJson();
	TestTrue(TEXT("and writes the row back with one key"), Row.Contains(TEXT("\"stone\":33"), ESearchCase::CaseSensitive) && !Row.Contains(TEXT("Stone"), ESearchCase::CaseSensitive));
	TestEqual(TEXT("a row that is not an object is a first-timer's"), FCubeInventory::Parse(TEXT("[1,2]")).Count(TEXT("dirt")), CubeSpec::StartingStack);
	TestEqual(TEXT("so is a broken one"), FCubeInventory::Parse(TEXT("{\"dirt\":")).Count(TEXT("dirt")), CubeSpec::StartingStack);
	TestEqual(TEXT("an empty object is an empty inventory"), FCubeInventory::Parse(TEXT("{}")).Total(), 0);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
