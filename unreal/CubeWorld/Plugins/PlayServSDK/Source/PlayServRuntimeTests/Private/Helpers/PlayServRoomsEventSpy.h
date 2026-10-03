#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "PlayServRoomsEventSpy.generated.h"

/**
 * A listener for the Rooms module's Blueprint-assignable events. They are DYNAMIC multicast
 * delegates — the SDK's convention for anything a Blueprint can bind — so a test cannot attach a
 * lambda to one: AddDynamic needs a UFUNCTION on a UObject. This is that UObject.
 */
UCLASS()
class UPlayServRoomsEventSpy : public UObject
{
	GENERATED_BODY()

public:
	/** Every OnPlayerRemoved, in order. */
	TArray<TTuple<FString, FString, FString>> Removed;

	UFUNCTION()
	void OnPlayerRemoved(const FString& RoomName, const FString& PlayerId, const FString& Reason)
	{
		Removed.Emplace(RoomName, PlayerId, Reason);
	}

	/** Every OnRoomEnded, in order. */
	TArray<TTuple<FString, FString>> Ended;

	UFUNCTION()
	void OnRoomEnded(const FString& RoomName, const FString& Reason)
	{
		Ended.Emplace(RoomName, Reason);
	}

	bool WasRemoved(const FString& PlayerId, const FString& Reason) const
	{
		for (const TTuple<FString, FString, FString>& Entry : Removed)
		{
			if (Entry.Get<1>() == PlayerId && Entry.Get<2>() == Reason)
			{
				return true;
			}
		}
		return false;
	}
};
