#pragma once

#include "CoreMinimal.h"

namespace PlayServRosterHash
{
	PLAYSERVRUNTIME_API FString Sha256Hex(const uint8* Data, int32 Length);

	PLAYSERVRUNTIME_API FString Sha256Hex(const FString& Utf8Source);

	PLAYSERVRUNTIME_API FString CanonicalRoster(const TArray<FString>& PlayerIds);

	PLAYSERVRUNTIME_API FString Compute(const TArray<FString>& PlayerIds);

	PLAYSERVRUNTIME_API int32 Count(const TArray<FString>& PlayerIds);
}
