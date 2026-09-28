using UnrealBuildTool;

public class CubeWorldTarget : TargetRules
{
	public CubeWorldTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("CubeWorld");
	}
}
