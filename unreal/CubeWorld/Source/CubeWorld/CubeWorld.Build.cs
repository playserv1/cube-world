using UnrealBuildTool;

public class CubeWorld : ModuleRules
{
	public CubeWorld(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core", "CoreUObject", "Engine", "InputCore",
			"ProceduralMeshComponent", "Json", "JsonUtilities", "Sockets", "Networking",
			"RenderCore", "RHI", "PlayServRuntime",
		});
	}
}
