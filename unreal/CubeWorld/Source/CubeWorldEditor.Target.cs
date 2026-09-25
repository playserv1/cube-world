using UnrealBuildTool;

public class CubeWorldEditorTarget : TargetRules
{
	public CubeWorldEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		IncludeOrderVersion = EngineIncludeOrderVersion.Latest;
		ExtraModuleNames.Add("CubeWorld");
	}
}
