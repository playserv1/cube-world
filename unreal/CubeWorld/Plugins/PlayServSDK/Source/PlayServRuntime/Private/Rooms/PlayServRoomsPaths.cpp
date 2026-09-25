#include "Rooms/PlayServRoomsPaths.h"
#include "GenericPlatform/GenericPlatformHttp.h"

namespace
{
	bool GUseMatchmakingTwins = false;

	FString Encode(const FString& Segment)
	{
		return FGenericPlatformHttp::UrlEncode(Segment);
	}
}

void PlayServRoomsPaths::SetUseMatchmakingTwins(bool bUseTwins)
{
	GUseMatchmakingTwins = bUseTwins;
}

bool PlayServRoomsPaths::UsesMatchmakingTwins()
{
	return GUseMatchmakingTwins;
}

FString PlayServRoomsPaths::List(const FString& ExecutorSlug)
{
	return GUseMatchmakingTwins
		? FString::Printf(TEXT("/matchmaking/%s/rooms"), *Encode(ExecutorSlug))
		: FString::Printf(TEXT("/rooms/%s"), *Encode(ExecutorSlug));
}

FString PlayServRoomsPaths::Browse(const FString& ExecutorSlug)
{
	return GUseMatchmakingTwins
		? FString::Printf(TEXT("/matchmaking/%s/rooms:browse"), *Encode(ExecutorSlug))
		: FString::Printf(TEXT("/rooms/%s:browse"), *Encode(ExecutorSlug));
}

FString PlayServRoomsPaths::Upsert(const FString& ExecutorSlug)
{
	return GUseMatchmakingTwins
		? FString::Printf(TEXT("/matchmaking/%s/rooms/upsert"), *Encode(ExecutorSlug))
		: FString::Printf(TEXT("/rooms/%s:upsert"), *Encode(ExecutorSlug));
}

FString PlayServRoomsPaths::Close(const FString& ExecutorSlug, const FString& RoomName)
{
	return GUseMatchmakingTwins
		? FString::Printf(TEXT("/matchmaking/%s/rooms/%s:close"), *Encode(ExecutorSlug), *Encode(RoomName))
		: FString::Printf(TEXT("/rooms/%s/%s:close"), *Encode(ExecutorSlug), *Encode(RoomName));
}

FString PlayServRoomsPaths::Config(const FString& ExecutorSlug)
{
	return GUseMatchmakingTwins
		? FString::Printf(TEXT("/matchmaking/%s/room-config"), *Encode(ExecutorSlug))
		: FString::Printf(TEXT("/rooms/%s/config"), *Encode(ExecutorSlug));
}

FString PlayServRoomsPaths::Join(const FString& ExecutorSlug, const FString& RoomName)
{
	return FString::Printf(TEXT("/rooms/%s/%s:join"), *Encode(ExecutorSlug), *Encode(RoomName));
}

FString PlayServRoomsPaths::Host(const FString& ExecutorSlug)
{
	return FString::Printf(TEXT("/rooms/%s:host"), *Encode(ExecutorSlug));
}

FString PlayServRoomsPaths::UplinkUrl(const FString& BaseUrl)
{
	FString Url = BaseUrl;
	Url.ReplaceInline(TEXT("https://"), TEXT("wss://"));
	Url.ReplaceInline(TEXT("http://"), TEXT("ws://"));
	Url.RemoveFromEnd(TEXT("/"));
	Url += TEXT("/uplink");
	return Url;
}
