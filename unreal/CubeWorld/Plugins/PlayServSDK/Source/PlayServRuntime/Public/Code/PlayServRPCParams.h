#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

/**
 * Parameters for an untyped cloud-function call.
 *
 * Usage:
 *   FPlayServRPCParams()
 *       .Set(TEXT("PlayerId"), PlayerId)
 *       .Set(TEXT("Amount"), 100)
 *       .Set(TEXT("Premium"), true)
 *
 * Keys are sent as written and must match the function's C# property names exactly, case included.
 */
class PLAYSERVRUNTIME_API FPlayServRPCParams
{
public:
	FPlayServRPCParams();

	FPlayServRPCParams& Set(const FString& Key, const FString& Value);
	FPlayServRPCParams& Set(const FString& Key, int32 Value);
	FPlayServRPCParams& Set(const FString& Key, int64 Value);
	FPlayServRPCParams& Set(const FString& Key, double Value);
	FPlayServRPCParams& Set(const FString& Key, bool Value);
	FPlayServRPCParams& Set(const FString& Key, const TSharedPtr<FJsonValue>& Value);
	FPlayServRPCParams& Set(const FString& Key, const TSharedPtr<FJsonObject>& Value);

	/** The parameters as a JSON object. */
	TSharedPtr<FJsonObject> ToJson() const;

private:
	TSharedPtr<FJsonObject> Params;
};
