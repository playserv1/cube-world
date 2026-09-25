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

		// The game socket to a C# server runs over OpenSSL's connect BIO, plain or TLS; the SSL module owns the certificate store.
		PrivateDependencyModuleNames.Add("SSL");
		// The keys and the mouse are read straight from the system around a border crossing (CubeKeys.cpp).
		PrivateDependencyModuleNames.AddRange(new string[] { "ApplicationCore", "Slate", "SlateCore" });
		AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL");
	}
}
