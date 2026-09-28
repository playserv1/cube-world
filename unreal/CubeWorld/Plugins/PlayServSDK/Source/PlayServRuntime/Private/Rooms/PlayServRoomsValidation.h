#pragma once

#include "CoreMinimal.h"
#include "Core/PlayServTypes.h"
#include "Rooms/PlayServRoomsTypes.h"

namespace PlayServRoomsValidation
{
	constexpr int32 MaxRoomNameLength = 64;
	constexpr int32 MaxInstanceIdLength = 64;
	constexpr int32 MaxRegionLength = 32;
	constexpr int32 MaxConnectStringLength = 512;
	constexpr int32 MaxAttributesBytes = 2048;

	constexpr int32 MaxCapacityAttribute = 1000;

	PLAYSERVRUNTIME_API bool IsValidRoomName(const FString& RoomName);

	PLAYSERVRUNTIME_API bool IsValidInstanceId(const FString& InstanceId);

	PLAYSERVRUNTIME_API bool IsValidRegion(const FString& Region);

	PLAYSERVRUNTIME_API int32 AttributesByteSize(const TMap<FString, FString>& Attributes);

	PLAYSERVRUNTIME_API int32 ParseCapacityAttribute(const FString& Text);

	bool IsValidConnect(const FPlayServRoomConnect& Connect, FString& OutReason);

	PLAYSERVRUNTIME_API FPlayServError ValidateSnapshot(const FPlayServRoomSnapshot& Snapshot);

	const TCHAR* TransportToWire(EPlayServRoomTransport Transport);

	PLAYSERVRUNTIME_API bool TransportFromWire(const FString& Wire, EPlayServRoomTransport& OutTransport);
}
