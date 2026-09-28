#include "PlayServRuntimeModule.h"
#include "Core/PlayServLog.h"

DEFINE_LOG_CATEGORY(LogPlayServ);

#define LOCTEXT_NAMESPACE "FPlayServRuntimeModule"

void FPlayServRuntimeModule::StartupModule()
{
}

void FPlayServRuntimeModule::ShutdownModule()
{
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FPlayServRuntimeModule, PlayServRuntime)
