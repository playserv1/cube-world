// The world's rules for a block whose row is deleted, checked without a platform: on the server it goes back to the
// generated terrain, the wire names that with its own kind, and a read of the table takes for gone only what it did
// not find and what did not change after it began.
#include "CubePhysics.h"
#include "CubeServerWorld.h"
#include "CubeSpec.h"
#include "CubeVoxelWorld.h"
#include "CubeWebSocketServer.h"
#include "CubeSocket.h"
#include "CubeWorldGameInstance.h"
#include "CubeWorldGameMode.h"
#include "Data/PlayServData.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/InputSettings.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonWriter.h"

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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCubeWorldArrivalTest,
	"CubeWorld.Crossing.AJumpFromTheServerListLandsInTheRegion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// A player another server just saw comes over the border when they were seen at it, and jumps from the server list when
// they were seen anywhere else: the next server used to place both where they were seen, so a jump sent the player back.
bool FCubeWorldArrivalTest::RunTest(const FString& Parameters)
{
	const int32 Blue = 1;   // x in [24, 48), y in [0, 24)
	double X = 25.1, Y = 6;
	TestTrue(TEXT("seen just inside, they walked in"), CubeCrossedInto(Blue, X, Y));
	TestTrue(TEXT("and stand where they were seen"), X == 25.1 && Y == 6);
	X = 22.5; Y = 6;
	TestTrue(TEXT("seen a step short of the border (the last pose lags), they walked in too"), CubeCrossedInto(Blue, X, Y));
	TestEqual(TEXT("and stand inside the region"), X, 24 + CubeSpec::Width / 2);
	X = 12; Y = 12;
	TestFalse(TEXT("seen in the middle of red, they jumped"), CubeCrossedInto(Blue, X, Y));
	X = 60; Y = 36;
	TestFalse(TEXT("seen in another row, they jumped"), CubeCrossedInto(Blue, X, Y));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCubeWorldRemoteHurtTest,
	"CubeWorld.Presence.AHitOnAnotherServersPlayerFlashesThemHere",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// A player another server hosts shows less health than a pose heard a moment ago: this server flashes them for its own
// players (their server tells only its own, PSV-2981). Rises, deaths, and poses too far apart or out of order are no hit.
bool FCubeWorldRemoteHurtTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("19 a fifth of a second after 20 is a hit"), CubeWasHurt(20, 1000, 19, 1200));
	TestFalse(TEXT("the same health is no hit"), CubeWasHurt(19, 1000, 19, 1200));
	TestFalse(TEXT("more health is a heal"), CubeWasHurt(18, 1000, 19, 1200));
	TestFalse(TEXT("a death shows as a tombstone, not a flash"), CubeWasHurt(3, 1000, 0, 1200));
	TestFalse(TEXT("a pose 5 s older may predate a stay here"), CubeWasHurt(20, 1000, 19, 6000));
	TestFalse(TEXT("an older pose heard late is no hit"), CubeWasHurt(20, 1200, 19, 1000));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCubeWorldEditRegionTest,
	"CubeWorld.Rules.AServerEditsOnlyNearItsRegion",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// A bot on red stood in purple without crossing and dug and placed there (PSV-2979): a server edits the world for a
// player in its region or a crossing's step past its border (InThisRegion).
bool FCubeWorldEditRegionTest::RunTest(const FString& Parameters)
{
	const int32 Blue = 1;   // x in [24, 48), y in [0, 24)
	TestTrue(TEXT("in its region"), CubeNear(Blue, 30, 10, CubeSpec::BorderSlack));
	TestTrue(TEXT("a step past its border, crossing to green"), CubeNear(Blue, 48 + CubeSpec::BorderSlack, 10, CubeSpec::BorderSlack));
	TestFalse(TEXT("deep in green"), CubeNear(Blue, 60, 10, CubeSpec::BorderSlack));
	TestFalse(TEXT("in purple, the other row"), CubeNear(Blue, 30, 40, CubeSpec::BorderSlack));
	TestFalse(TEXT("a server with no region is near none"), CubeNear(-1, 30, 10, CubeSpec::BorderSlack));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCubeWorldStuckPastBorderTest,
	"CubeWorld.Rules.APlayerWhoseNextRoomIsNotUpCanOnlyWalkThere",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// A player who walked over a border into a room that was down stayed with the old server and dug, placed and took bombs
// there: past the border the old server serves them only for a crossing's moment, and only into a live server's region.
bool FCubeWorldStuckPastBorderTest::RunTest(const FString& Parameters)
{
	const int32 Yellow = 3;   // x in [0, 24), y in [24, 48); purple, 4, is beside it
	const TArray<int32> PurpleUp = { 4 }, NobodyElse;
	TestTrue(TEXT("in its region"), CubeServes(Yellow, 12, 36, 0, NobodyElse));
	TestTrue(TEXT("in its region, however long"), CubeServes(Yellow, 12, 36, 60000, NobodyElse));
	TestTrue(TEXT("crossing into a live purple"), CubeServes(Yellow, 26.5, 36, 0, PurpleUp));
	TestTrue(TEXT("crossing into a live purple, at the end of the moment"), CubeServes(Yellow, 26.5, 36, CubeSpec::CrossingMs, PurpleUp));
	TestFalse(TEXT("purple's room did not let them in"), CubeServes(Yellow, 26.5, 36, CubeSpec::CrossingMs + 1, PurpleUp));
	TestFalse(TEXT("no live server holds purple"), CubeServes(Yellow, 26.5, 36, 0, NobodyElse));
	TestFalse(TEXT("deep in purple"), CubeServes(Yellow, 31.5, 36, 0, PurpleUp));
	TestFalse(TEXT("a server with no region"), CubeServes(-1, 12, 36, 0, PurpleUp));
	TestTrue(TEXT("a block of its own"), CubeServesBlock(Yellow, 23, 36, NobodyElse));
	TestTrue(TEXT("a block of a live purple"), CubeServesBlock(Yellow, 24, 36, PurpleUp));
	TestFalse(TEXT("a block of a purple nobody holds"), CubeServesBlock(Yellow, 24, 36, NobodyElse));
	TestFalse(TEXT("a server with no region, a block of nobody's"), CubeServesBlock(-1, 23, 36, NobodyElse));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCubeWorldHitFreshTest,
	"CubeWorld.Server.AHitLandsOnlyWithinFiveSecondsOfBeingWritten",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// The table keeps every hit whose victim had moved on (116 rows on dev on 2026-10-02): heard again when the victim is
// back, or read again on a new subscription, such a hit must not land then. HearHit on the C# side.
bool FCubeWorldHitFreshTest::RunTest(const FString& Parameters)
{
	const int64 Written = 1790943259592;
	TestTrue(TEXT("a hit heard as it is written lands"), CubeHitIsFresh(Written, Written + 20));
	TestTrue(TEXT("so does one 4.9 s old"), CubeHitIsFresh(Written, Written + 4900));
	TestFalse(TEXT("one 5 s old does not, as on the C# servers"), CubeHitIsFresh(Written, Written + 5000));
	TestFalse(TEXT("nor one left in the table an hour ago"), CubeHitIsFresh(Written, Written + 3600000));
	TestFalse(TEXT("nor one with no time"), CubeHitIsFresh(0, Written));

	// A C# server's hit as the uplink brings it: its fields, with `at` a number of milliseconds.
	TSharedPtr<FJsonObject> Row;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(TEXT("{\"hit_id\":\"plr_A:21054:a12b\",\"victim\":\"plr_B\",\"attacker\":\"plr_A\",\"damage\":1,\"kx\":1,\"ky\":0,\"strength\":0.4,\"at\":1790937352580}")), Row);
	const FCubeHitRecord Hit = ACubeWorldGameMode::HitOf(Row);
	TestEqual(TEXT("its id"), Hit.Id, FString(TEXT("plr_A:21054:a12b")));
	TestEqual(TEXT("its victim"), Hit.Victim, FString(TEXT("plr_B")));
	TestEqual(TEXT("its strength"), Hit.Strength, 0.4);
	TestEqual(TEXT("its time, to the millisecond"), Hit.At, (int64)1790937352580);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCubeWorldPoseOrderTest,
	"CubeWorld.Server.APoseHeardLateDoesNotTakeThePlayerBack",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// The platform sends each change of a row on its own, so two poses a moment apart can arrive the other way round.
bool FCubeWorldPoseOrderTest::RunTest(const FString& Parameters)
{
	// A C# server's pose as the uplink brings it: sneaking and sprinting are 0 or 1, seen_at milliseconds.
	TSharedPtr<FJsonObject> Row;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(TEXT("{\"player_id\":\"plr_H\",\"name\":\"Artem\",\"server\":\"xcbtb\",\"color\":\"pink\",\"x\":60,\"y\":36,\"z\":-0.77,\"yaw\":1.46,\"pitch\":0.04,\"health\":17,\"sneaking\":1,\"sprinting\":0,\"seen_at\":1790947383745}")), Row);
	const FCubeElsewhere Heard = ACubeWorldGameMode::ElsewhereOf(Row);
	TestEqual(TEXT("the player"), Heard.Pose.Id, FString(TEXT("plr_H")));
	TestEqual(TEXT("their server"), Heard.Pose.Server, FString(TEXT("xcbtb")));
	TestEqual(TEXT("their health"), (double)Heard.Pose.Health, 17.0);
	TestTrue(TEXT("sneaking"), Heard.Pose.bSneaking);
	TestFalse(TEXT("not sprinting"), Heard.Pose.bSprinting);
	TestEqual(TEXT("seen at"), Heard.SeenAt, (int64)1790947383745);

	TSharedPtr<FJsonObject> Old;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(TEXT("{\"player_id\":\"plr_T\",\"seen_at\":1790310020905}")), Old);
	TestEqual(TEXT("a row written before health was (dev has one) reads as whole"), (double)ACubeWorldGameMode::ElsewhereOf(Old).Pose.Health, 20.0);

	TMap<FString, FCubeElsewhere> Elsewhere;
	auto Pose = [](double X, double Health, int64 SeenAt) { FCubeElsewhere E; E.Pose.Id = TEXT("plr_H"); E.Pose.X = X; E.Pose.Health = Health; E.SeenAt = SeenAt; return E; };
	TestTrue(TEXT("a first pose is taken"), ACubeWorldGameMode::MergePose(Elsewhere, Pose(10, 20, 1000)) == ECubePoseHeard::Taken);
	TestTrue(TEXT("a newer one too"), ACubeWorldGameMode::MergePose(Elsewhere, Pose(11, 20, 1200)) == ECubePoseHeard::Taken);
	TestTrue(TEXT("an older one heard late is not"), ACubeWorldGameMode::MergePose(Elsewhere, Pose(10, 20, 1000)) == ECubePoseHeard::Older);
	TestEqual(TEXT("and the player stays where the newer one put them"), (double)Elsewhere.FindChecked(TEXT("plr_H")).Pose.X, 11.0);
	TestTrue(TEXT("less health a moment later is a hurt to flash"), ACubeWorldGameMode::MergePose(Elsewhere, Pose(11, 19, 1400)) == ECubePoseHeard::Hurt);
	TestTrue(TEXT("the late pose that showed them whole flashes nothing"), ACubeWorldGameMode::MergePose(Elsewhere, Pose(11, 20, 1300)) == ECubePoseHeard::Older);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCubeWorldPresenceHandOffTest,
	"CubeWorld.Server.APlayerWhoCrossesIsHandedOverWithoutAGap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// Blue keeps the pose of a player who left it, and red writes them a moment later. A delete of a row of blue's after that
// (one left over from an older race) takes out nothing; the delete of the server that holds them does (PSV-3018).
bool FCubeWorldPresenceHandOffTest::RunTest(const FString& Parameters)
{
	TMap<FString, FCubeElsewhere> Elsewhere;
	auto Pose = [](const TCHAR* Server, double X, int64 SeenAt) { FCubeElsewhere E; E.Pose.Id = TEXT("plr_W"); E.Pose.Server = Server; E.Pose.X = X; E.SeenAt = SeenAt; return E; };
	TestTrue(TEXT("the old server keeps the last pose it saw"), ACubeWorldGameMode::MergePose(Elsewhere, Pose(TEXT("blue"), 24.2, 1000)) == ECubePoseHeard::Taken);
	TestTrue(TEXT("the next server's first pose takes its place"), ACubeWorldGameMode::MergePose(Elsewhere, Pose(TEXT("red"), 23.9, 1300)) == ECubePoseHeard::Taken);
	TestFalse(TEXT("a late delete of the old server's row takes out nothing"), ACubeWorldGameMode::DeleteTakesOut(Elsewhere.Find(TEXT("plr_W")), TEXT("blue")));
	TestTrue(TEXT("the delete of the server that holds them takes them out"), ACubeWorldGameMode::DeleteTakesOut(Elsewhere.Find(TEXT("plr_W")), TEXT("red")));
	TestTrue(TEXT("so does a delete that names no server"), ACubeWorldGameMode::DeleteTakesOut(Elsewhere.Find(TEXT("plr_W")), FString()));
	TestFalse(TEXT("a delete of a player nobody knows takes out nothing"), ACubeWorldGameMode::DeleteTakesOut(Elsewhere.Find(TEXT("plr_X")), TEXT("red")));
	TestEqual(TEXT("the row is kept as long as the C# servers keep theirs"), CubeLeaveGraceMs, (int64)2000);
	TestEqual(TEXT("and written as often as they write theirs, 20 times a second (PSV-3015)"), CubePresenceWriteSeconds, 0.05f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCubeWorldBombGoneFromTableTest,
	"CubeWorld.Server.ABombTheTableNoLongerHasGoesOutOfPlay",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// On dev on 2026-10-02 red and green kept 24 bombs whose fizzles the platform lost: by the next read of the table the drop
// function had swept their rows, and nothing ended them (PSV-2977). Two reads that both find no row of a bomb end it.
bool FCubeWorldBombGoneFromTableTest::RunTest(const FString& Parameters)
{
	const int64 T = 1790955000000;
	auto Bomb = [](const TCHAR* State, int64 DroppedAt) { FCubeLiveBomb B; B.Record.State = State; B.Record.DroppedAt = DroppedAt; return B; };
	TMap<FString, FCubeLiveBomb> InPlay;
	InPlay.Add(TEXT("ghost"), Bomb(TEXT("free"), T - 42 * 60000));
	InPlay.Add(TEXT("held-ghost"), Bomb(TEXT("held"), T - 20 * 60000));
	InPlay.Add(TEXT("live"), Bomb(TEXT("free"), T - 60000));
	InPlay.Add(TEXT("fresh"), Bomb(TEXT("free"), T - 2000));
	const TSet<FString> Table = { TEXT("live"), TEXT("held-elsewhere") };
	TMap<FString, int64> Missing;

	TestEqual(TEXT("one read with no row of a bomb takes nothing out"), ACubeWorldGameMode::BombsGoneFromTable(InPlay, Table, Missing, T).Num(), 0);
	TestTrue(TEXT("but marks the bombs it had no row of"), Missing.Num() == 2 && Missing.Contains(TEXT("ghost")) && Missing.Contains(TEXT("held-ghost")));
	TestFalse(TEXT("not one dropped a moment ago, whose row may not be in a read yet"), Missing.Contains(TEXT("fresh")));
	TestEqual(TEXT("a second read a moment later takes nothing out yet"), ACubeWorldGameMode::BombsGoneFromTable(InPlay, Table, Missing, T + 100).Num(), 0);
	TestEqual(TEXT("and keeps when they were first missed"), Missing.FindRef(TEXT("ghost")), T);

	const TArray<FString> Gone = ACubeWorldGameMode::BombsGoneFromTable(InPlay, Table, Missing, T + CubeBombRecheckMs);
	TestTrue(TEXT("a read 5 s on with no row of them takes them out, free or held"), Gone.Num() == 2 && Gone.Contains(TEXT("ghost")) && Gone.Contains(TEXT("held-ghost")));
	TestEqual(TEXT("and nothing stays marked"), Missing.Num(), 0);

	Missing.Reset();
	Missing.Add(TEXT("ghost"), T);
	const TSet<FString> Found = { TEXT("live"), TEXT("ghost") };
	TestEqual(TEXT("a bomb a later read finds a row of is not taken out"), ACubeWorldGameMode::BombsGoneFromTable(InPlay, Found, Missing, T + CubeBombRecheckMs).Num(), 0);
	TestFalse(TEXT("and is no longer marked"), Missing.Contains(TEXT("ghost")));

	Missing.Reset();
	Missing.Add(TEXT("ghost"), T);
	TestEqual(TEXT("a read that found no row at all takes nothing out"), ACubeWorldGameMode::BombsGoneFromTable(InPlay, TSet<FString>(), Missing, T + CubeBombRecheckMs).Num(), 0);
	TestEqual(TEXT("and marks nothing"), Missing.Num(), 0);

	ACubeWorldGameMode::BombsGoneFromTable(InPlay, Table, Missing, T + CubeBombNoRowGraceMs + 1000);
	TestTrue(TEXT("a bomb dropped longer ago than the grace is marked too"), Missing.Contains(TEXT("fresh")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCubeWorldBombWithNoRowIsNotHandedOutTest,
	"CubeWorld.Server.ABombWhoseEndWasMissedIsNotHandedOutAndGoesWithinSeconds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// PSV-3032: a bomb whose fizzle the uplink never brought stayed free on an Unreal server, drawn and offered to the players
// in reach, until the uplink happened to reconnect; the table was read again only then. Now it is read every few seconds.
// From the read that finds no row of a bomb on, nobody is handed it, and a read CubeBombRecheckMs later takes it out.
bool FCubeWorldBombWithNoRowIsNotHandedOutTest::RunTest(const FString& Parameters)
{
	const int64 T = 1791052000000;
	const int32 Red = 0, Blue = 1;
	auto Record = [](const TCHAR* State, double X, double Y, int64 DroppedAt)
	{
		FCubeBombRecord R; R.State = State; R.X = X; R.Y = Y; R.DroppedAt = DroppedAt; R.At = DroppedAt;
		return R;
	};
	auto InPlayAs = [](const FCubeBombRecord& R) { FCubeLiveBomb B; B.Record = R; return B; };
	// Red's region is x 0-24, y 0-24; both lie two blocks in from its border with yellow, as the drop puts them now.
	const FCubeBombRecord Live = Record(TEXT("free"), 21, 22, T - 60000);
	const FCubeBombRecord Stale = Record(TEXT("free"), 22, 21, T - 4 * 60000);
	TMap<FString, int64> Missing;

	TestTrue(TEXT("the table is read again within a few seconds, not only when the uplink reconnects"), CubeBombReadBackMs <= 5000);
	TestTrue(TEXT("a free bomb over this server's region is handed out"), ACubeWorldGameMode::HandsOut(TEXT("live"), Live, Red, Missing));
	TestTrue(TEXT("so is one whose end this server missed, until a read of the table shows it"), ACubeWorldGameMode::HandsOut(TEXT("stale"), Stale, Red, Missing));
	TestFalse(TEXT("not one over another server's region"), ACubeWorldGameMode::HandsOut(TEXT("live"), Live, Blue, Missing));
	TestFalse(TEXT("nor one already held"), ACubeWorldGameMode::HandsOut(TEXT("live"), Record(TEXT("held"), 21, 22, T - 60000), Red, Missing));

	// The fizzle of "stale" was lost and the drop function has swept its rows: the next read finds none of them.
	TMap<FString, FCubeLiveBomb> InPlay;
	InPlay.Add(TEXT("live"), InPlayAs(Live));
	InPlay.Add(TEXT("stale"), InPlayAs(Stale));
	const TSet<FString> Table = { TEXT("live"), TEXT("drop-elsewhere") };
	TestEqual(TEXT("the read that first finds no row of it takes nothing out"), ACubeWorldGameMode::BombsGoneFromTable(InPlay, Table, Missing, T).Num(), 0);
	TestFalse(TEXT("but from then on nobody is handed it"), ACubeWorldGameMode::HandsOut(TEXT("stale"), Stale, Red, Missing));
	TestTrue(TEXT("while a bomb the read found a row of is handed out as before"), ACubeWorldGameMode::HandsOut(TEXT("live"), Live, Red, Missing));
	TestEqual(TEXT("a read a moment later takes nothing out yet"), ACubeWorldGameMode::BombsGoneFromTable(InPlay, Table, Missing, T + 1000).Num(), 0);
	TestFalse(TEXT("and still hands it to nobody"), ACubeWorldGameMode::HandsOut(TEXT("stale"), Stale, Red, Missing));
	const TArray<FString> Gone = ACubeWorldGameMode::BombsGoneFromTable(InPlay, Table, Missing, T + CubeBombRecheckMs);
	TestTrue(TEXT("the read CubeBombRecheckMs on takes it out of play"), Gone.Num() == 1 && Gone[0] == TEXT("stale"));

	// A bomb a read missed for a moment (its row came back in the next one) is handed out again.
	Missing.Reset();
	Missing.Add(TEXT("live"), T);
	ACubeWorldGameMode::BombsGoneFromTable(InPlay, { TEXT("live"), TEXT("stale") }, Missing, T + 1000);
	TestTrue(TEXT("a bomb the next read found a row of is handed out again"), ACubeWorldGameMode::HandsOut(TEXT("live"), Live, Red, Missing));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCubeWorldKeysetPagingTest,
	"CubeWorld.Data.AFullReadPagesByRecordId",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// A full read of WorldCube is about 80 pages. The platform's cursor skips a count of rows in updated_at order, so the rows
// deleted while a read ran moved others past a page: on dev on 2026-10-02 two Unreal servers lost 39 and 288 blocks in one
// read (PSV-3014). Each page asks for the records after the last record id of the page before, in record-id order.
bool FCubeWorldKeysetPagingTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FJsonObject> Base;
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(TEXT("{\"filters\":[{\"field\":\"x\",\"op\":\"gte\",\"value\":48}],\"limit\":200,\"cursor\":\"eyJvIjoyMDB9\"}")), Base);
	const auto SortedById = [](const TSharedPtr<FJsonObject>& Body)
	{
		const TArray<TSharedPtr<FJsonValue>>* Sort;
		return Body->TryGetArrayField(TEXT("sort"), Sort) && Sort->Num() == 1
			&& (*Sort)[0]->AsObject()->GetStringField(TEXT("field")) == TEXT("id") && (*Sort)[0]->AsObject()->GetStringField(TEXT("dir")) == TEXT("asc");
	};

	const TSharedPtr<FJsonObject> First = UPlayServData::KeysetPageBody(Base, FString());
	TestTrue(TEXT("the first page is sorted by record id"), SortedById(First));
	TestFalse(TEXT("and asks for no cursor"), First->HasField(TEXT("cursor")));
	TestEqual(TEXT("its filters are the read's own"), First->GetArrayField(TEXT("filters")).Num(), 1);
	TestEqual(TEXT("and so is its page size"), (int32)First->GetNumberField(TEXT("limit")), 200);

	const TSharedPtr<FJsonObject> Next = UPlayServData::KeysetPageBody(Base, TEXT("rec_009VJWN3ZCKQTJF2QR7Q1NVHMD"));
	const TArray<TSharedPtr<FJsonValue>>& Filters = Next->GetArrayField(TEXT("filters"));
	TestEqual(TEXT("the next page adds one filter"), Filters.Num(), 2);
	const TSharedPtr<FJsonObject> After = Filters.Last()->AsObject();
	TestTrue(TEXT("for the records after the last one of the page before"), After->GetStringField(TEXT("field")) == TEXT("id")
		&& After->GetStringField(TEXT("op")) == TEXT("gt") && After->GetStringField(TEXT("value")) == TEXT("rec_009VJWN3ZCKQTJF2QR7Q1NVHMD"));
	TestTrue(TEXT("still sorted by record id"), SortedById(Next));
	TestEqual(TEXT("the read's own filters stay as they were"), Base->GetArrayField(TEXT("filters")).Num(), 1);
	TestEqual(TEXT("a read with no filters gets only that one"), UPlayServData::KeysetPageBody(MakeShared<FJsonObject>(), TEXT("rec_A"))->GetArrayField(TEXT("filters")).Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCubeWorldDoorCloseTest,
	"CubeWorld.Wire.TheDoorAnswersABareCloseWithANormalOne",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// A browser's close() sends no status code; answering with none reached it as 1005, where the C# servers answer 1000.
bool FCubeWorldDoorCloseTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("a bare close is answered with 1000"), FCubeWebSocketServer::CloseAnswer({}) == TArray<uint8>({ 0x03, 0xE8 }));
	TestTrue(TEXT("a close with a code is answered with that code"), FCubeWebSocketServer::CloseAnswer({ 0x0F, 0xA1, 'b', 'y', 'e' }) == TArray<uint8>({ 0x0F, 0xA1 }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCubeWorldSocketFrameOrderTest,
	"CubeWorld.Wire.ALargeFrameIsParsedOffTheGameThreadInTurn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// A C# server's welcome carries the whole world (2.4 MB on dev), and parsing it on the game thread took a frame of
// 115 ms at every crossing into a C# room: it is parsed on a worker, and the frames behind it wait for it, so the game
// still reads every frame in the order the server sent them.
bool FCubeWorldSocketFrameOrderTest::RunTest(const FString& Parameters)
{
	const TSharedRef<FCubeSocket> Socket = MakeShared<FCubeSocket>(TEXT("localhost"), 1, false);
	Socket->State = FCubeSocket::EState::Open;
	const uint32 GameThread = FPlatformTLS::GetCurrentThreadId();
	uint32 WorldReadOn = GameThread;
	Socket->Decode = [&WorldReadOn](FCubeSocketFrame& F) { if (F.Json->HasField(TEXT("world"))) WorldReadOn = FPlatformTLS::GetCurrentThreadId(); };
	TArray<FString> Order;
	Socket->OnFrame.AddLambda([&Order](FCubeSocketFrame& F) { Order.Add(F.Json->GetStringField(TEXT("type"))); });
	const auto Utf8 = [](const FString& S) { const FTCHARToUTF8 U(*S); return TArray<uint8>((const uint8*)U.Get(), U.Length()); };
	FString Welcome = TEXT("{\"type\":\"welcome\",\"world\":[");
	for (int32 I = 0; I < 5000; I++) Welcome += FString::Printf(TEXT("%s{\"x\":%d,\"y\":1,\"z\":0,\"kind\":\"stone\"}"), I ? TEXT(",") : TEXT(""), I);
	Welcome += TEXT("]}");

	Socket->ReceiveText(Utf8(TEXT("{\"type\":\"first\"}")));
	TestEqual(TEXT("a small frame with nothing ahead of it goes out at once"), Order.Num(), 1);
	Socket->ReceiveText(Utf8(Welcome));
	Socket->ReceiveText(Utf8(TEXT("{\"type\":\"behind\"}")));
	TestEqual(TEXT("the frames behind a large one wait for it"), Order.Num(), 1);
	// Polled, not waited for: a wait may run a task that has not started on the waiting thread.
	const double Deadline = FPlatformTime::Seconds() + 10;
	auto AllParsed = [&Socket]() { for (const FCubeSocket::FPending& P : Socket->Pending) if (!P.Parse.IsCompleted()) return false; return true; };
	while (!AllParsed() && FPlatformTime::Seconds() < Deadline) FPlatformProcess::Sleep(0.001f);
	Socket->Deliver(false);
	TestTrue(TEXT("every frame goes out, in the order they came"), Order == TArray<FString>({ TEXT("first"), TEXT("welcome"), TEXT("behind") }));
	TestNotEqual(TEXT("the large one was read on a worker"), WorldReadOn, GameThread);

	// The game's own decoder: the welcome's blocks come out as a cell map, and out of the JSON, so their JSON is freed on
	// the worker and not by the game thread after the frame.
	FCubeSocketFrame Parsed;
	Parsed.Json = MakeShared<FJsonObject>();
	FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(TEXT("{\"type\":\"welcome\",\"world\":[{\"x\":1,\"y\":2,\"z\":3,\"kind\":\"stone\"},{\"x\":4,\"y\":5,\"z\":6,\"kind\":\"Stone\"}]}")), Parsed.Json);
	UCubeWorldGameInstance::DecodeSocketFrame(Parsed);
	TestTrue(TEXT("the welcome's blocks are read"), Parsed.World.IsSet() && Parsed.World->Num() == 2 && Parsed.World->FindRef(FIntVector(4, 5, 6)) == TEXT("stone"));
	TestFalse(TEXT("and dropped from its JSON"), Parsed.Json->HasField(TEXT("world")));
	TestEqual(TEXT("the rest of the frame stays"), Parsed.Json->GetStringField(TEXT("type")), FString(TEXT("welcome")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCubeWorldMoveCheckTest,
	"CubeWorld.Moves.NoFasterThanASprintJump",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

// The move check both servers apply (World.cs MoveCheck on the C# side), against the client's own physics.
bool FCubeWorldMoveCheckTest::RunTest(const FString& Parameters)
{
	const FCubeSolidQuery Flat = [](int32, int32, int32 Z) { return Z < 0; };
	FCubeBody Body;
	Body.Teleport(2, 10, 0);
	FCubeMoveCheck Check;
	Check.Reset(Body.X, Body.Y, Body.Z, 0);
	FCubeInput Run;
	Run.Forward = 1; Run.bSprint = true; Run.bJump = true; Run.Yaw = -UE_DOUBLE_HALF_PI;
	bool bAllAccepted = true;
	int64 T = 0;
	for (int32 I = 0; I < 180 && Body.X < 68; I++)
	{
		CubePhysics::Tick(Body, Run, Flat);
		T += 50;
		bAllAccepted &= Check.Check(Body.X, Body.Y, Body.Z, TOptional<int32>(), T) == ECubeMoveVerdict::Accepted;
	}
	TestTrue(TEXT("a sprint-jump along the world is accepted all the way"), bAllAccepted && Body.X > 30);

	// Knocked away in the air by a full blast, then falling: the push and the fall are allowed.
	CubePhysics::Knockback(Body, -1, 0, 1);
	Check.Knocked(1);
	FCubeInput Idle;
	bAllAccepted = true;
	for (int32 I = 0; I < 40; I++)
	{
		CubePhysics::Tick(Body, Idle, Flat);
		T += 50;
		bAllAccepted &= Check.Check(Body.X, Body.Y, Body.Z, TOptional<int32>(), T) == ECubeMoveVerdict::Accepted;
	}
	TestTrue(TEXT("a knockback is accepted"), bAllAccepted);

	FCubeMoveCheck Jump;
	Jump.Reset(10, 10, 0, 0);
	TestTrue(TEXT("forty blocks in one move is refused"), Jump.Check(50, 10, 0, TOptional<int32>(0), 1000) == ECubeMoveVerdict::Refused);
	TestTrue(TEXT("and the player stays put"), Jump.X == 10 && Jump.Seq == 1);
	TestTrue(TEXT("a move sent before the correction was taken is dropped"), Jump.Check(51, 10, 0, TOptional<int32>(0), 1050) == ECubeMoveVerdict::Stale);
	TestTrue(TEXT("the next one is checked again"), Jump.Check(10.3, 10, 0, TOptional<int32>(1), 1100) == ECubeMoveVerdict::Accepted);

	FCubeMoveCheck Old;
	Old.Reset(10, 10, 0, 0);
	Old.Check(40, 10, 0, TOptional<int32>(), 3000);
	TestTrue(TEXT("a client that cannot take a correction is taken where it says after a second"), Old.Check(40.4, 10, 0, TOptional<int32>(), 4000) == ECubeMoveVerdict::Accepted && Old.X == 40.4);

	FCubeMoveCheck Crossed;
	Crossed.Arrive(12, 36, 0, 3, 0);
	TestTrue(TEXT("a player who crossed plays on where they stand, though the server guessed the region's middle"), Crossed.Check(12, 22.5, 0, TOptional<int32>(0), 50) == ECubeMoveVerdict::Accepted && Crossed.Y == 22.5);
	FCubeMoveCheck FarOff;
	FarOff.Arrive(12, 36, 0, 3, 0);
	TestTrue(TEXT("a first move from far off the region is put back to the guess"), FarOff.Check(60, 5, 0, TOptional<int32>(0), 50) == ECubeMoveVerdict::Refused && FarOff.Y == 36);

	FCubeMoveCheck Climb;
	Climb.Reset(10, 10, 0, 0);
	TestTrue(TEXT("a climb of twenty blocks in one move is refused"), Climb.Check(10, 10, 20, TOptional<int32>(), 50) == ECubeMoveVerdict::Refused);
	FCubeMoveCheck Fall;
	Fall.Reset(10, 10, 40, 0);
	TestTrue(TEXT("a fall of forty is free"), Fall.Check(10, 10, 0, TOptional<int32>(), 50) == ECubeMoveVerdict::Accepted);

	TestTrue(TEXT("yellow is near its border"), CubeNear(3, 26.5, 36, CubeSpec::BorderSlack));
	TestFalse(TEXT("green is not near yellow"), CubeNear(3, 60.5, 5.5, CubeSpec::BorderSlack));
	return true;
}

// ── the player's own view at a crossing (PSV-3027) ───────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCubeWorldGapStartsFromTheDrawnViewTest,
	"CubeWorld.Camera.TheGapStartsFromTheViewAsDrawn",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCubeWorldGapStartsFromTheDrawnViewTest::RunTest(const FString& Parameters)
{
	UCubeWorldGameInstance* Game = NewObject<UCubeWorldGameInstance>();
	// The last game tick took the body and the view; the player turned on in the frames since, and each frame noted the
	// view it drew. The gap starts from that one, not from the tick's: 50 ms of turning at 300 degrees a second is 15.
	const FRotator Ticked(-5.f, 160.f, 0), Drawn(-11.5f, 172.25f, 0);
	Game->LastBody = { true, 10.0, 12.0, 0.0, CubeSpec::YawFromUnreal(Ticked.Yaw), CubeSpec::PitchFromUnreal(Ticked.Pitch) };
	Game->NoteDrawnView(Drawn);
	const FRotator Look = UCubeWorldGameInstance::LookOf(Game->LastBody);
	TestTrue(TEXT("the gap looks the way the last frame was drawn"), FMath::IsNearlyZero(FRotator::NormalizeAxis(Look.Yaw - Drawn.Yaw), 1e-3f));
	TestTrue(TEXT("up and down too"), FMath::IsNearlyEqual(Look.Pitch, Drawn.Pitch, 1e-3f));
	TestTrue(TEXT("the body is the game tick's still"), Game->LastBody.X == 10.0 && Game->LastBody.Y == 12.0);
	// Unreal keeps a view looking down as 360 minus the angle: the gap takes it as the same view.
	Game->NoteDrawnView(FRotator(348.5f, -10.f, 0));
	const FRotator Down = UCubeWorldGameInstance::LookOf(Game->LastBody);
	TestTrue(TEXT("a view looking down is the same view"), FMath::IsNearlyEqual(Down.Pitch, -11.5f, 1e-3f) && FMath::IsNearlyZero(FRotator::NormalizeAxis(Down.Yaw + 10.f), 1e-3f));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCubeWorldFovNeverLockedTest,
	"CubeWorld.Camera.TheFieldOfViewIsNeverLeftLocked",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCubeWorldFovNeverLockedTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
	FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
	Context.SetCurrentWorld(World);
	APlayerCameraManager* Cam = World->SpawnActor<APlayerCameraManager>();
	TestNotNull(TEXT("a camera manager"), Cam);
	if (Cam)
	{
		// The gap draws the camera manager's own view at the pawn's field of view: as its default, never as a lock, which
		// the camera manager would keep for the pawn after the gap, whatever the pawn's camera said.
		UCubeWorldGameInstance::HoldFov(Cam, 112.8f);
		TestEqual(TEXT("the gap draws the pawn's field of view"), Cam->DefaultFOV, 112.8f);
		TestEqual(TEXT("and leaves it unlocked"), Cam->GetLockedFOV(), 0.f);
		TestEqual(TEXT("so what is drawn is the view's own"), Cam->GetFOVAngle(), Cam->GetCameraCacheView().FOV);
		// A lock from anywhere else does not outlive the next gap either.
		Cam->SetFOV(96.5f);
		UCubeWorldGameInstance::HoldFov(Cam, 101.6f);
		TestEqual(TEXT("a lock left from before is released"), Cam->GetLockedFOV(), 0.f);
	}
	GEngine->DestroyWorldContext(World);
	World->DestroyWorld(false);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCubeWorldGapTurnsAsThePawnTest,
	"CubeWorld.Camera.TheGapTurnsAsThePawnDoes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCubeWorldGapTurnsAsThePawnTest::RunTest(const FString& Parameters)
{
	const UInputSettings* Settings = GetDefault<UInputSettings>();
	TestTrue(TEXT("the project scales the mouse with the field of view (DefaultInput.ini)"), Settings->bEnableFOVScaling);
	// What UPlayerInput gives the pawn's Turn for 100 pixels while 101.6 degrees are drawn (70 vertical at 16:9).
	const float Pawn = 100.f * Settings->FOVScale * 101.6f * CubeSpec::DegreesPerMousePixel;
	TestTrue(TEXT("the gap turns as far as the pawn would"), FMath::IsNearlyEqual(UCubeWorldGameInstance::MouseDegrees(100.f, 101.6f), Pawn, 1e-4f));
	TestTrue(TEXT("further at the sprint's wider view, as the pawn does"), UCubeWorldGameInstance::MouseDegrees(100.f, 112.8f) > UCubeWorldGameInstance::MouseDegrees(100.f, 101.6f));
	TestEqual(TEXT("no field of view known yet: unscaled"), UCubeWorldGameInstance::MouseDegrees(100.f, 0.f), 15.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCubeWorldFakeHandTest,
	"CubeWorld.Camera.TheFakeHandSwingsAndTheWalkKeepsItsHeading",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCubeWorldFakeHandTest::RunTest(const FString& Parameters)
{
	// -fakemouse=300: 30 degrees each way at 300 a second, there at 0.1 s, back through the middle at 0.2 s.
	TestTrue(TEXT("the swing starts in the middle"), FMath::IsNearlyZero(UCubeWorldGameInstance::SwingAt(0, 300, 30), 1e-6));
	TestTrue(TEXT("at the rate"), FMath::IsNearlyEqual(UCubeWorldGameInstance::SwingAt(0.05, 300, 30), 15.0, 1e-6));
	TestTrue(TEXT("as far as 30"), FMath::IsNearlyEqual(UCubeWorldGameInstance::SwingAt(0.1, 300, 30), 30.0, 1e-6));
	TestTrue(TEXT("back through the middle"), FMath::IsNearlyZero(UCubeWorldGameInstance::SwingAt(0.2, 300, 30), 1e-6));
	TestTrue(TEXT("as far the other way"), FMath::IsNearlyEqual(UCubeWorldGameInstance::SwingAt(0.3, 300, 30), -30.0, 1e-6));
	TestTrue(TEXT("and round again"), FMath::IsNearlyZero(UCubeWorldGameInstance::SwingAt(0.4, 300, 30), 1e-6));

	// The walk's keys go the heading's way whichever way the view looks (Unreal yaw: 0 is +x, 90 is +y).
	TestTrue(TEXT("ahead: forward"), UCubeWorldGameInstance::WalkKeys(90, 90).Equals(FVector2D(1, 0), 1e-6));
	TestTrue(TEXT("to the view's right: strafing right (Minecraft's strafe is positive to the left)"), UCubeWorldGameInstance::WalkKeys(90, 0).Equals(FVector2D(0, -1), 1e-6));
	TestTrue(TEXT("behind: backwards"), UCubeWorldGameInstance::WalkKeys(180, 0).Equals(FVector2D(-1, 0), 1e-6));
	// And the body goes that way: one tick with the keys for +y while the view looks along +x.
	const FVector2D Keys = UCubeWorldGameInstance::WalkKeys(90, 0);
	FCubeBody Body;
	Body.Teleport(10, 10, 0);
	Body.bOnGround = true;
	FCubeInput Input;
	Input.Forward = Keys.X; Input.Strafe = Keys.Y; Input.Yaw = CubeSpec::YawFromUnreal(0);
	CubePhysics::Tick(Body, Input, [](int32, int32, int32 Z) { return Z < 0; });
	TestTrue(TEXT("the body walks +y"), Body.Y > 10 && FMath::IsNearlyEqual(Body.X, 10.0, 1e-6));
	return true;
}

// ── presence over the uplink (PSV-3028) ──────────────────────────────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCubeWorldPresenceRowTest,
	"CubeWorld.Presence.TheRowGoesOutAsTheCSharpServersWriteIt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCubeWorldPresenceRowTest::RunTest(const FString& Parameters)
{
	FCubePresenceRep P;
	P.Id = TEXT("plr_1"); P.Name = TEXT("Ann"); P.Server = TEXT("ck7g8"); P.Color = TEXT("blue");
	P.X = 23.5f; P.Y = 9.f; P.Z = 0.25f; P.Yaw = 1.5f; P.Pitch = -0.25f; P.Health = 17.f; P.bSneaking = false; P.bSprinting = true;
	const TSharedRef<FJsonObject> Row = ACubeWorldGameMode::PresenceJson(P, 1790979996713);
	// Every field of WorldPresence as the C# servers declare and write it (CubeWorld.Server/World.cs), and no other: an
	// upsert merges what it carries, so a field left out would keep whatever the last writer put there.
	const TArray<FString> Fields = { TEXT("player_id"), TEXT("name"), TEXT("server"), TEXT("color"), TEXT("x"), TEXT("y"), TEXT("z"),
		TEXT("yaw"), TEXT("pitch"), TEXT("health"), TEXT("sneaking"), TEXT("sprinting"), TEXT("seen_at") };
	TestEqual(TEXT("as many fields as the table has"), Row->Values.Num(), Fields.Num());
	for (const FString& Field : Fields) TestTrue(FString::Printf(TEXT("%s is there"), *Field), Row->HasField(Field));
	TestEqual(TEXT("the key is the player"), Row->GetStringField(TEXT("player_id")), FString(TEXT("plr_1")));
	TestEqual(TEXT("sprinting as the integer the table holds"), Row->GetNumberField(TEXT("sprinting")), 1.0);
	TestEqual(TEXT("sneaking too"), Row->GetNumberField(TEXT("sneaking")), 0.0);
	// The table's integers must reach the platform as integers: seen_at is a time in milliseconds, 13 digits.
	FString Text;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
	FJsonSerializer::Serialize(Row, Writer);
	TestTrue(TEXT("seen_at goes out as a plain integer"), Text.Contains(TEXT("\"seen_at\":1790979996713")));
	TestTrue(TEXT("and so do the flags"), Text.Contains(TEXT("\"sprinting\":1")) && Text.Contains(TEXT("\"sneaking\":0")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FCubeWorldPresenceTakenOverTest,
	"CubeWorld.Presence.ALeaversRowGoesUnlessAnotherServerTookThemOver",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FCubeWorldPresenceTakenOverTest::RunTest(const FString& Parameters)
{
	FCubeElsewhere Ours;
	Ours.Pose.Id = TEXT("plr_1"); Ours.Pose.Server = TEXT("ck7g8"); Ours.SeenAt = 1000;
	TestFalse(TEXT("nobody heard of since: the row goes"), ACubeWorldGameMode::TakenOver(nullptr, Ours));
	TestFalse(TEXT("only the pose this server kept: the row goes"), ACubeWorldGameMode::TakenOver(&Ours, Ours));
	FCubeElsewhere Next = Ours;
	Next.Pose.Server = TEXT("8we6h"); Next.SeenAt = 1300;
	TestTrue(TEXT("the next server wrote them after they left: they are its now, and so is the row"), ACubeWorldGameMode::TakenOver(&Next, Ours));
	FCubeElsewhere Before = Next;
	Before.SeenAt = 900;
	TestFalse(TEXT("a pose another server wrote before they came here does not count"), ACubeWorldGameMode::TakenOver(&Before, Ours));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
