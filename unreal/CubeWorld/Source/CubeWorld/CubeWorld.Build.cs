using UnrealBuildTool;

public class CubeWorld : ModuleRules
{
	public CubeWorld(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		// Every source file compiles on its own: several define the same small helpers (Num, Str, ToText) and constants
		// in their anonymous namespaces, which clash in a unity file. The Launcher's engine builds a game module this small
		// file by file anyway; a source-built engine (the one with the Linux Server target) put it in one unity file, and
		// neither clang nor MSVC compiled it.
		bUseUnity = false;

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
