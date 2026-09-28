#include "PlayServPushSchemasCommandlet.h"
#include "PlayServSchemaPusher.h"
#include "Misc/Parse.h"

DEFINE_LOG_CATEGORY_STATIC(LogPlayServPushCommandlet, Display, All);

int32 UPlayServPushSchemasCommandlet::Main(const FString& Params)
{
	FString ManifestPath;
	FParse::Value(*Params, TEXT("-ManifestPath="), ManifestPath);
	FString ServerKey;
	FParse::Value(*Params, TEXT("-PlayServServerKey="), ServerKey);
	FString BaseURL;
	FParse::Value(*Params, TEXT("-PlayServBaseURL="), BaseURL);
	FString OverlayPath;
	FParse::Value(*Params, TEXT("-OverlayPath="), OverlayPath);

	const bool bDryRun = FParse::Param(*Params, TEXT("PlayServDryRun"));

	const FPlayServSchemaPusher::FResult Result = FPlayServSchemaPusher::Run(ManifestPath, ServerKey, BaseURL, bDryRun, OverlayPath);

	if (Result.bSuccess)
	{
		UE_LOG(LogPlayServPushCommandlet, Display, TEXT("PlayServ schema push succeeded.\n%s"), *Result.Summary);
		return 0;
	}

	UE_LOG(LogPlayServPushCommandlet, Error, TEXT("PlayServ schema push FAILED: %s"), *Result.Summary);
	return 1;
}
