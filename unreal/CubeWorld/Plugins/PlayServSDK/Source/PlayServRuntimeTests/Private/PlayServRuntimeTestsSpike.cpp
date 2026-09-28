#include "Misc/AutomationTest.h"
#include "Data/PlayServData.h"
#include "SpikeTestEntities.h"

#if !UE_BUILD_SHIPPING

// ---------------------------------------------------------------------------
// PlayServ.RuntimeTests.Spike.*
//
// Answers "can a plugin-side automation module host the SDK test suite?" with
// the two mechanisms everything else rides on:
//   - ModuleDiscovery: the automation framework finds tests registered by a
//     module that lives inside the plugin (Type: UncookedOnly — loads in the
//     editor and uncooked targets, never in a customer's cooked build).
//   - CodegenFromPluginModule: entity detection works for classes DECLARED in
//     this module — the PlayServUht specifier route resolves through the same
//     public predicate the whole serializer/change-tracking stack funnels
//     through.
//
// Synchronous, no backend, no game types.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServRuntimeTestsModuleDiscoverySpike,
	"PlayServ.RuntimeTests.Spike.ModuleDiscovery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServRuntimeTestsModuleDiscoverySpike::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("a test registered from the plugin-side module is discovered and runs"), true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServRuntimeTestsCodegenRouteSpike,
	"PlayServ.RuntimeTests.Spike.CodegenFromPluginModule",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServRuntimeTestsCodegenRouteSpike::RunTest(const FString& Parameters)
{
	TestTrue(
		TEXT("UCLASS(PlayServEntity) declared in the plugin test module is detected (specifier route)"),
		UPlayServData::IsRegisteredPersistentClass(USpikeCodegenEntity::StaticClass()));

	// (A second specifier fixture — still proves in-plugin-module detection.)
	TestTrue(
		TEXT("second specifier entity declared in the plugin test module is detected"),
		UPlayServData::IsRegisteredPersistentClass(USpikeAncestryEntity::StaticClass()));

	TestFalse(
		TEXT("plain UObject is not detected (control)"),
		UPlayServData::IsRegisteredPersistentClass(UObject::StaticClass()));

	return true;
}

#endif // !UE_BUILD_SHIPPING
