#include "Core/PlayServVersion.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/AutomationTest.h"

#if !UE_BUILD_SHIPPING

// ---------------------------------------------------------------------------
// PlayServ.Core.Version.DescriptorMatchesHeader  (PSV-2647)
//
// The version is written by hand in two files: PlayServVersion.h, whose PLAYSERV_SDK_VERSION the
// SDK announces to the platform on every realtime handshake, and the plugin descriptor's
// VersionName, which is what a customer sees in the Plugins window. They have disagreed before
// (0.5.0-dev in the descriptor while the wire said 0.5.0), so the suite holds them together.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServVersionDescriptorMatchesHeaderTest,
	"PlayServ.Core.Version.DescriptorMatchesHeader",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServVersionDescriptorMatchesHeaderTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("PlayServSDK"));
	if (!TestTrue(TEXT("the PlayServSDK plugin is loaded"), Plugin.IsValid()))
	{
		return false;
	}
	TestEqual(TEXT("PlayServSDK.uplugin VersionName is the version the SDK announces"), Plugin->GetDescriptor().VersionName, FString(PLAYSERV_SDK_VERSION));
	return true;
}

#endif
