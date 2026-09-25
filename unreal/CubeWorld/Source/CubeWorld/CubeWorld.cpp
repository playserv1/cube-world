#include "CubeWorld.h"
#include "Modules/ModuleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

DEFINE_LOG_CATEGORY(LogCubeWorld);

IMPLEMENT_PRIMARY_GAME_MODULE(FDefaultGameModuleImpl, CubeWorld, "CubeWorld");

bool CubeIsServerProcess()
{
	static const bool bHeadless = FParse::Param(FCommandLine::Get(), TEXT("cubeserver"));
	return IsRunningDedicatedServer() || bHeadless;
}
