using UnrealBuildTool;

public class CubeWorld : ModuleRules
{
	public CubeWorld(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core", "CoreUObject", "Engine", "InputCore", "NetCore",
			"ProceduralMeshComponent", "Json", "JsonUtilities", "Sockets", "Networking", "WebSockets",
			"RenderCore", "RHI", "PlayServRuntime",
		});

		// The dedicated server replicates over Iris (net.Iris.UseIrisReplication=1 in DefaultEngine.ini).
		SetupIrisSupport(Target);
	}
}
