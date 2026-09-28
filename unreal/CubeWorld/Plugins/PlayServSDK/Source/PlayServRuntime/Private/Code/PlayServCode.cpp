#include "Code/PlayServCode.h"
#include "Code/PlayServRPCParams.h"
#include "Core/PlayServHttp.h"
#include "Core/PlayServLog.h"

void UPlayServCode::Init(TSharedPtr<FPlayServHttp> InHttp)
{
	Http = InHttp;
}

void UPlayServCode::Shutdown()
{
	Http.Reset();
}

void UPlayServCode::Call(const FString& FunctionName, const FPlayServRPCParams& Params, FPlayServRPCCallback Callback)
{
	if (!Http.IsValid())
	{
		Callback.ExecuteIfBound(false, FPlayServRPCResult(), FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("PlayServ HTTP not initialized")));
		return;
	}

	if (FunctionName.IsEmpty())
	{
		UE_LOG(LogPlayServ, Warning, TEXT("PlayServ Code: Call rejected — empty function name"));
		Callback.ExecuteIfBound(false, FPlayServRPCResult(), FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("Function name cannot be empty")));
		return;
	}

	const FString Endpoint = TEXT("/fn/") + FunctionName;
	TSharedPtr<FJsonObject> Body = Params.ToJson();

	UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ Code: Call %s"), *FunctionName);

	Http->Request(EPlayServHttpVerb::Post, Endpoint, Body, FPlayServV2Callback::CreateLambda(
		[Callback, FunctionName](bool bSuccess, const FPlayServHttpResponse& Response, const FPlayServError& Error)
		{
			if (!bSuccess)
			{
				UE_LOG(LogPlayServ, Warning, TEXT("PlayServ Code: Call %s failed — %s"), *FunctionName, *Error.Message);
			}
			FPlayServRPCResult Payload;
			Payload.Response = Response.Json;
			Callback.ExecuteIfBound(bSuccess, Payload, Error);
		}));
}
