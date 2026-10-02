#include "CubeWorld.h"
#include "Modules/ModuleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/NetworkVersion.h"
#include "Misc/Parse.h"
#include "HAL/IConsoleManager.h"
#include "Runtime/Launch/Resources/Version.h"
#include "CubeSpec.h"

DEFINE_LOG_CATEGORY(LogCubeWorld);

namespace
{
	/** Engine/Build/Build.version of UE 5.8 (the Launcher's): the changelist its network version is compatible with. */
	constexpr int32 Ue58CompatibleChangelist = 55116800;
	static_assert(ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION == 8, "a new engine release: put its CompatibleChangelist above");
}

// Every UE 5.8 build of Cube World answers with the same network version, whichever engine built it. The stock version
// hashes the engine's compatible changelist: a Launcher build has the release's, a source-built engine has none (0).
// The Linux Server target comes only from a source-built engine, so such a server turned away every Launcher-built
// client as outdated ("an incompatible version of the game"). The protocol itself is still checked: the engine's and
// the game's network protocol versions stay in the hash.
class FCubeWorldModule : public FDefaultGameModuleImpl
{
public:
	virtual void StartupModule() override
	{
		if (FNetworkVersion::GetNetworkCompatibleChangelist() != 0) return;   // a Launcher build, or -networkversionoverride=
		if (IConsoleVariable* Override = IConsoleManager::Get().FindConsoleVariable(TEXT("networkversionoverride")))
		{
			Override->Set(Ue58CompatibleChangelist, ECVF_SetByCode);
			FNetworkVersion::InvalidateNetworkChecksum();
			UE_LOG(LogCubeWorld, Log, TEXT("network version: this engine has no changelist; the release's, %d, as the Launcher's builds"), Ue58CompatibleChangelist);
		}
	}
};

IMPLEMENT_PRIMARY_GAME_MODULE(FCubeWorldModule, CubeWorld, "CubeWorld");

bool CubeIsServerProcess()
{
	return IsRunningDedicatedServer();
}

bool CubeIsOffline()
{
	static const bool bOffline = FParse::Param(FCommandLine::Get(), TEXT("cubeoffline"));
	return bOffline;
}

const TArray<FCubeOfflinePeer>& CubeOfflinePeers()
{
	static const TArray<FCubeOfflinePeer> Peers = []()
	{
		TArray<FCubeOfflinePeer> Out;
		FString List;
		if (!FParse::Value(FCommandLine::Get(), TEXT("-peers="), List, false)) return Out;
		TArray<FString> Items;
		List.ParseIntoArray(Items, TEXT(","));
		for (const FString& Item : Items)
		{
			FString RegionText, Address;
			if (!Item.Split(TEXT("@"), &RegionText, &Address) || Address.IsEmpty()) continue;
			Out.Add({ FCString::Atoi(*RegionText), Address });
		}
		return Out;
	}();
	return Peers;
}

FString CubeOfflineRoomName(int32 Region)
{
	return FString::Printf(TEXT("%s-offline"), CubeSpec::RegionColorName(Region));
}
