#pragma once

#include "CoreMinimal.h"

namespace PlayServRoomsPaths
{
	PLAYSERVRUNTIME_API void SetUseMatchmakingTwins(bool bUseTwins);
	PLAYSERVRUNTIME_API bool UsesMatchmakingTwins();

	PLAYSERVRUNTIME_API FString List(const FString& ExecutorSlug);

	PLAYSERVRUNTIME_API FString Browse(const FString& ExecutorSlug);

	PLAYSERVRUNTIME_API FString Upsert(const FString& ExecutorSlug);

	PLAYSERVRUNTIME_API FString Close(const FString& ExecutorSlug, const FString& RoomName);

	PLAYSERVRUNTIME_API FString Config(const FString& ExecutorSlug);

	PLAYSERVRUNTIME_API FString Join(const FString& ExecutorSlug, const FString& RoomName);

	PLAYSERVRUNTIME_API FString Host(const FString& ExecutorSlug);

	PLAYSERVRUNTIME_API FString UplinkUrl(const FString& BaseUrl);
}
