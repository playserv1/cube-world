using UnrealBuildTool;

// Editor-only tooling for the PlayServ SDK: the schema pusher (commandlet + Tools-menu
// action). Never ships in a cooked build.
public class PlayServEditor : ModuleRules
{
	public PlayServEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"HTTP",
			"Json",
			"Projects",
			"ToolMenus",
			"Slate",
			"SlateCore",
			"UnrealEd",
			"PlayServRuntime",
		});
	}
}
