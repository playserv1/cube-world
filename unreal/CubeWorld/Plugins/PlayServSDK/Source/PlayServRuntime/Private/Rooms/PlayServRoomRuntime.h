#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Rooms/PlayServRoomsTypes.h"

enum class EPlayServRosterCheck : uint8
{
	None,
	Ok,
	Mismatch
};

struct PLAYSERVRUNTIME_API FPlayServUpsertAck
{
	bool bCreated = false;
	EPlayServRosterCheck RosterCheck = EPlayServRosterCheck::None;
	EPlayServPlacementState Placement = EPlayServPlacementState::Unknown;
	bool bOpenRefused = false;
	bool bHasRoomConfig = false;
	FPlayServRoomConfig RoomConfig;

	static bool Parse(const TSharedPtr<FJsonObject>& Json, FPlayServUpsertAck& OutAck);
	static EPlayServPlacementState ParsePlacementState(const FString& Wire);
};

struct PLAYSERVRUNTIME_API FPlayServRoomRuntime
{
	FPlayServRoomSnapshot Snapshot;

	TSet<FString> Roster;

	TMap<FString, double> Parked;

	TMap<FString, FString> JoinTokens;

	EPlayServPlacementState Placement = EPlayServPlacementState::Unknown;

	bool bRegistered = false;
	bool bClosing = false;

	double StartedAt = 0.0;
	double NextBeatAt = 0.0;
	double IdleSince = 0.0;

	int64 LastSeq = 0;
	int32 InFlightBeats = 0;
	int32 ConsecutiveFailures = 0;
	int32 RosterSentForGeneration = 0;

	TArray<FString> RosterIds() const;
	int32 PlayerCount() const { return Roster.Num(); }

	int64 NextSeq(int64 UnixMilliseconds);
};

namespace PlayServRoomRuntime
{
	TSharedPtr<FJsonObject> BuildConnectObject(const FPlayServRoomConnect& Connect);

	PLAYSERVRUNTIME_API TSharedPtr<FJsonObject> BuildUpsertBody(const FPlayServRoomRuntime& Room, const FString& InstanceId);

	PLAYSERVRUNTIME_API TSharedPtr<FJsonObject> BuildPresenceDelta(const FString& RoomName, int64 Seq, const TCHAR* Event, const FString& PlayerId, const FString& ReservationToken);

	PLAYSERVRUNTIME_API TSharedPtr<FJsonObject> BuildRosterFrame(const FString& RoomName, int64 Seq, const TArray<FString>& PlayerIds);

	int64 UnixMillisecondsNow();
}
