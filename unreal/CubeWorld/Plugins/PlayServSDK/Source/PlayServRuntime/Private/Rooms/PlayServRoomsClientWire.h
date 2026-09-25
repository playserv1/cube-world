#pragma once

#include "CoreMinimal.h"
#include "Core/PlayServTypes.h"
#include "Dom/JsonObject.h"
#include "Rooms/PlayServRoomsTypes.h"

namespace PlayServRoomsClientWire
{
	static constexpr double ConnectPollIntervalSeconds = 1.0;

	static constexpr float JoinTimeoutSeconds = 45.0f;

	static constexpr float ConnectWaitSeconds = 30.0f;

	static constexpr int32 MaxRetries = 3;

	static constexpr float HostTimeoutSeconds = 120.0f;

	const TCHAR* PlacementStateToWire(EPlayServPlacementState State);

	EPlayServPlacementState PlacementStateFromWire(const FString& Wire);

	PLAYSERVRUNTIME_API FString BuildBrowseQuery(const FPlayServRoomFilters& Filters);

	bool ParseConnect(const TSharedPtr<FJsonObject>& Json, const FString& Field, FPlayServRoomConnect& OutConnect);

	void ParseAttributes(const TSharedPtr<FJsonObject>& Json, const FString& Field, TMap<FString, FString>& OutAttributes);

	PLAYSERVRUNTIME_API bool ParseAttributesJson(const FString& Text, TMap<FString, FString>& OutAttributes);

	PLAYSERVRUNTIME_API TSharedPtr<FJsonObject> BuildHostBody(const TMap<FString, FString>& Attributes);

	PLAYSERVRUNTIME_API bool ParseHostedRoom(const TSharedPtr<FJsonObject>& Json, FPlayServRoomListing& OutRoom);

	PLAYSERVRUNTIME_API bool ParseBrowsePage(const TSharedPtr<FJsonObject>& Json, FPlayServBrowsePage& OutPage);

	PLAYSERVRUNTIME_API bool ParseMatched(const TSharedPtr<FJsonObject>& Json, const FString& DateHeader, double ReceivedAtMonotonic, FPlayServRoomTicket& OutTicket);

	PLAYSERVRUNTIME_API FString BuildTravelUrl(const FPlayServRoomTicket& Ticket);

	enum class EJoinStep : uint8
	{
		JoinNamedRoom,
		Deliver,
		Stop
	};

	struct FJoinLoopState
	{
		double ElapsedSeconds = 0.0;
		int32 Retries = 0;
		double ConnectWaitedSeconds = 0.0;
	};

	struct FJoinStepDecision
	{
		EJoinStep Step = EJoinStep::Stop;
		double DelaySeconds = 0.0;
		FPlayServError Error;
		bool bSpendsRetry = false;
	};

	struct FJoinAnswer
	{
		bool bSuccess = false;
		FString ProblemCode;
		int32 RetryAfterSeconds = 0;
		FPlayServError Error;
		bool bHasTicket = false;
		bool bTicketHasConnect = false;
	};

	PLAYSERVRUNTIME_API FJoinStepDecision DecideAfterJoin(const FJoinLoopState& State, const FJoinAnswer& Answer);
}
