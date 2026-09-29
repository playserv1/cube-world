using UnrealBuildTool;

// The dedicated server. A Server target needs an engine built from source (the Launcher's engine ships no
// UnrealServer binaries); with the Launcher's engine, run the server as the editor:
//   UnrealEditor-Cmd.exe CubeWorld.uproject -server -log -port=7777
public class CubeWorldServerTarget : TargetRules
{
	public CubeWorldServerTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Server;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("CubeWorld");
	}
}
