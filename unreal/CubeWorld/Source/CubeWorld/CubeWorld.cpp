#include "CubeWorld.h"
#include "Modules/ModuleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "CubeSpec.h"

DEFINE_LOG_CATEGORY(LogCubeWorld);

IMPLEMENT_PRIMARY_GAME_MODULE(FDefaultGameModuleImpl, CubeWorld, "CubeWorld");

bool CubeIsServerProcess()
{
	static const bool bHeadless = FParse::Param(FCommandLine::Get(), TEXT("cubeserver"));
	return IsRunningDedicatedServer() || bHeadless;
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
