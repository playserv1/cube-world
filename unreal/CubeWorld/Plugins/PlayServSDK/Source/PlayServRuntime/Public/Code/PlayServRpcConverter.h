#pragma once

#include "CoreMinimal.h"
#include "Code/PlayServRPCParams.h"
#include "Dom/JsonObject.h"
#include "UObject/Class.h"

/**
 * The USTRUCT and JSON conversion behind the typed PlayServ::Code::Call; most games never call it directly.
 *
 * It is strict: a field present with the wrong JSON type fails the whole conversion, and nothing is coerced ("10" is not
 * 10). Property names are sent as declared and must match the function's C# property names; a reply's fields are
 * matched to them ignoring case. Supported: FString, bool, int32, int64, float, double, enums (by name), uint8, nested
 * USTRUCTs and TArrays of these. Other types, and Transient and editor-only properties, are skipped. An int64 goes
 * through a double, so values beyond 2^53 lose precision.
 */
namespace PlayServ::Code
{
	/** Write a USTRUCT into cloud-function parameters, one per property. False only when Struct or Value is null. */
	PLAYSERVRUNTIME_API bool StructToRpcParams(const UStruct* Struct, const void* Value, FPlayServRPCParams& OutParams);

	/** Read a JSON object into a USTRUCT. False when a present field has the wrong type or an argument is null; a missing field keeps its C++ default. */
	PLAYSERVRUNTIME_API bool JsonObjectToStruct(const TSharedPtr<FJsonObject>& Json, const UStruct* Struct, void* OutValue);
}
