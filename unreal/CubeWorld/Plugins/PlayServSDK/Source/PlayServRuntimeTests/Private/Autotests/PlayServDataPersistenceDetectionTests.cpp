#include "PlayServTestCommon.h"
#include "Data/PlayServData.h"
#include "TestEntities.h"

#if !UE_BUILD_SHIPPING

// ---------------------------------------------------------------------------
// PlayServ.Data.Persistence.AncestryDetection
//
// Verifies the persistence-detection truth table that UPlayServData::IsRegisteredPersistentClass
// implements. Detection has exactly ONE source: the compile-time codegen descriptor from
// UCLASS(PlayServEntity) / meta=(PlayServEntity). Every persistence gate in the SDK — entity-ref-as-string-ID
// serialization, ref hydration, bulk Save/Delete eligibility, and change tracking — funnels
// through this single predicate, so a class-level check here proves the contract for all of
// them at once. The test NAME keeps its historical "AncestryDetection" leaf so suite history
// stays comparable.
//
//   plain UObject, specifier            → detected (compile-time codegen descriptor)
//   plain UObject, meta=() spelling     → detected (same route)
//   plain UObject, unmarked             → NOT detected
//
// Synchronous — no login / backend, only static class detection.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServPersistenceAncestryDetectionTest,
	"PlayServ.Data.Persistence.AncestryDetection",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServPersistenceAncestryDetectionTest::RunTest(const FString& Parameters)
{
	// (1) Specifier-marked plain UObject — detected via the codegen descriptor.
	TestTrue(
		TEXT("Plain UObject with UCLASS(PlayServEntity) is detected via codegen"),
		UPlayServData::IsRegisteredPersistentClass(UTestSpecifierEntity::StaticClass()));

	// (2) meta=(PlayServEntity) spelling — identical behavior to the bare specifier.
	TestTrue(
		TEXT("Plain UObject with UCLASS(meta=(PlayServEntity)) is detected via codegen"),
		UPlayServData::IsRegisteredPersistentClass(UTestSpecifierMetaEntity::StaticClass()));

	// (3) Plain unmarked UObject — not detected.
	TestFalse(
		TEXT("Plain unmarked UObject is not detected"),
		UPlayServData::IsRegisteredPersistentClass(UTestDetectPlainNoMacro::StaticClass()));

	// (4) A specifier-marked fixture with a historical name — detected via codegen.
	TestTrue(
		TEXT("Reparented UTestDetectBaseDerived is detected via codegen"),
		UPlayServData::IsRegisteredPersistentClass(UTestDetectBaseDerived::StaticClass()));

	// Guard the regression the whole CRUD / Collection / Queue suite leans on: UTestPlayer is
	// detected via the specifier.
	TestTrue(
		TEXT("Reparented UTestPlayer is detected via codegen"),
		UPlayServData::IsRegisteredPersistentClass(UTestPlayer::StaticClass()));

	// Null is never persistent (defensive — the predicate guards against it).
	TestFalse(
		TEXT("nullptr class is not detected"),
		UPlayServData::IsRegisteredPersistentClass(nullptr));

	return true;
}

#endif // !UE_BUILD_SHIPPING
