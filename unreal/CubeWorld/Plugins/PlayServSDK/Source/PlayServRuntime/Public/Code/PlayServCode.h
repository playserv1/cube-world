#pragma once

#include "CoreMinimal.h"
#include "Core/PlayServTypes.h"
#include "Code/PlayServRPCParams.h"
#include "Dom/JsonObject.h"
#include "PlayServCode.generated.h"

/** The reply of an untyped cloud-function call. */
USTRUCT(BlueprintType)
struct PLAYSERVRUNTIME_API FPlayServRPCResult
{
	GENERATED_BODY()

	/** The function's reply as JSON; null when it returned nothing. */
	TSharedPtr<FJsonObject> Response;
};

/** The callback of the untyped PlayServ::Code::Call. */
DECLARE_DELEGATE_ThreeParams(FPlayServRPCCallback, bool /*bSuccess*/, const FPlayServRPCResult& /*Result*/, const FPlayServError& /*Error*/);

/**
 * Cloud functions: calls to the C# functions deployed to your project. Games use PlayServ::Code::Call (PlayServ.h), which
 * has the typed forms; this class is UPlayServSubsystem::Get()->GetCode().
 */
UCLASS()
class PLAYSERVRUNTIME_API UPlayServCode : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Call a cloud function with parameters built at runtime; the same as the untyped PlayServ::Code::Call.
	 *
	 * @param FunctionName  The function's slug.
	 * @param Params        The parameters, built with FPlayServRPCParams.
	 * @param Callback      The reply, or the error.
	 */
	void Call(const FString& FunctionName, const FPlayServRPCParams& Params, FPlayServRPCCallback Callback);

private:
	friend class UPlayServSubsystem;
	void Init(TSharedPtr<class FPlayServHttp> InHttp);
	void Shutdown();

private:
	TSharedPtr<class FPlayServHttp> Http;
};
