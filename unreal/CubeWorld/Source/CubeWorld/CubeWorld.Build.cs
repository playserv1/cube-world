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

		// The game socket runs over OpenSSL's connect BIO, plain or TLS; the SSL module owns the certificate store.
		PrivateDependencyModuleNames.Add("SSL");
		AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL");
	}
}
