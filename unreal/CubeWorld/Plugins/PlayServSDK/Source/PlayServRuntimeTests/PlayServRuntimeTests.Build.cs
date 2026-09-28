using System.IO;
using UnrealBuildTool;

public class PlayServRuntimeTests : ModuleRules
{
	public PlayServRuntimeTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		// Test files include the shared helpers by bare name ("PlayServTestCommon.h"),
		// mirroring the include style they had in the game module.
		PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "Private", "Helpers"));

		// The credential-plane decision lives in PlayServRuntime/Private/Core/PlayServHttp.h
		// and must be asserted against the real transport, not a fake (see
		// PlayServ.Auth.V2.LoginSendsNoPlayerBearer). Same plugin, so a private include is fine.
		PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "..", "PlayServRuntime", "Private"));

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"HTTP",
			"Json",
			"OnlineSubsystem",
			"PlayServRuntime",
			"Projects",
			"WebSockets"
		});
	}
}
