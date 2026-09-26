using UnrealBuildTool;

// The dedicated server, which is what the machine pool runs (RUNBOOK.md Part E). A Server target needs an engine built
// from source (the Launcher's engine ships no UnrealServer binaries); on a developer's machine with the Launcher's
// engine, the editor serves instead: UnrealEditor-Cmd.exe CubeWorld.uproject -server -log -port=7777
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
