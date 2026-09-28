#include "Rooms/PlayServRoomRuntime.h"
#include "Rooms/PlayServRoomsValidation.h"
#include "Rooms/PlayServRoomsWire.h"
#include "Rooms/PlayServRosterHash.h"
#include "Rooms/PlayServUplinkClient.h"
#include "Dom/JsonValue.h"
#include "Misc/DateTime.h"

EPlayServPlacementState FPlayServUpsertAck::ParsePlacementState(const FString& Wire)
{
	if (Wire == TEXT("open"))
	{
		return EPlayServPlacementState::Open;
	}
	if (Wire == TEXT("full"))
	{
		return EPlayServPlacementState::Full;
	}
	if (Wire == TEXT("self_closed"))
	{
		return EPlayServPlacementState::SelfClosed;
	}
	if (Wire == TEXT("draining"))
	{
		return EPlayServPlacementState::Draining;
	}
	if (Wire == TEXT("session_closing"))
	{
		return EPlayServPlacementState::SessionClosing;
	}
	return EPlayServPlacementState::Unknown;
}

bool FPlayServUpsertAck::Parse(const TSharedPtr<FJsonObject>& Json, FPlayServUpsertAck& OutAck)
{
	if (!Json.IsValid())
	{
		return false;
	}
	FPlayServUpsertAck Ack;
	Json->TryGetBoolField(PlayServRoomsWire::FieldCreated, Ack.bCreated);

	FString RosterCheck;
	if (Json->TryGetStringField(PlayServRoomsWire::FieldRosterCheck, RosterCheck))
	{
		Ack.RosterCheck = RosterCheck == PlayServRoomsWire::RosterCheckMismatch ? EPlayServRosterCheck::Mismatch : EPlayServRosterCheck::Ok;
	}

	const TSharedPtr<FJsonObject>* Placement = nullptr;
	if (Json->TryGetObjectField(PlayServRoomsWire::FieldPlacement, Placement))
	{
		FString State;
		if ((*Placement)->TryGetStringField(PlayServRoomsWire::FieldState, State))
		{
			Ack.Placement = ParsePlacementState(State);
		}
		(*Placement)->TryGetBoolField(TEXT("open_refused"), Ack.bOpenRefused);
	}

	const TSharedPtr<FJsonObject>* Config = nullptr;
	if (Json->TryGetObjectField(PlayServRoomsWire::FieldRoomConfig, Config))
	{
		Ack.bHasRoomConfig = FPlayServUplinkAck::ParseRoomConfig(*Config, Ack.RoomConfig);
	}
	OutAck = Ack;
	return true;
}

TArray<FString> FPlayServRoomRuntime::RosterIds() const
{
	return Roster.Array();
}

int64 FPlayServRoomRuntime::NextSeq(int64 UnixMilliseconds)
{
	LastSeq = FMath::Max(LastSeq + 1, UnixMilliseconds);
	return LastSeq;
}

TSharedPtr<FJsonObject> PlayServRoomRuntime::BuildConnectObject(const FPlayServRoomConnect& Connect)
{
	TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(PlayServRoomsWire::FieldHost, Connect.Host);
	Object->SetNumberField(PlayServRoomsWire::FieldPort, Connect.Port);
	Object->SetStringField(PlayServRoomsWire::FieldTransport, PlayServRoomsValidation::TransportToWire(Connect.Transport));
	if (!Connect.ConnectString.IsEmpty())
	{
		Object->SetStringField(PlayServRoomsWire::FieldConnectString, Connect.ConnectString);
	}
	if (!Connect.Region.IsEmpty())
	{
		Object->SetStringField(PlayServRoomsWire::FieldRegion, Connect.Region);
	}
	return Object;
}

TSharedPtr<FJsonObject> PlayServRoomRuntime::BuildUpsertBody(const FPlayServRoomRuntime& Room, const FString& InstanceId)
{
	const FPlayServRoomSnapshot& Snapshot = Room.Snapshot;
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(PlayServRoomsWire::FieldRoomName, Snapshot.RoomName);
	Body->SetNumberField(PlayServRoomsWire::FieldPlayersCount, Room.PlayerCount());
	Body->SetNumberField(PlayServRoomsWire::FieldCapacity, Snapshot.Capacity);
	if (!Snapshot.State.IsEmpty())
	{
		Body->SetStringField(PlayServRoomsWire::FieldState, Snapshot.State);
	}
	Body->SetBoolField(PlayServRoomsWire::FieldOpen, Snapshot.bOpen);

	TSharedPtr<FJsonObject> Attributes = MakeShared<FJsonObject>();
	for (const TPair<FString, FString>& Pair : Snapshot.Attributes)
	{
		Attributes->SetStringField(Pair.Key, Pair.Value);
	}
	Body->SetObjectField(PlayServRoomsWire::FieldAttributes, Attributes);

	if (Snapshot.Connect.IsSet())
	{
		Body->SetObjectField(PlayServRoomsWire::FieldConnect, BuildConnectObject(Snapshot.Connect));
	}
	if (!Snapshot.Region.IsEmpty())
	{
		Body->SetStringField(PlayServRoomsWire::FieldRegion, Snapshot.Region);
	}
	Body->SetStringField(PlayServRoomsWire::FieldInstanceId, InstanceId);

	const TArray<FString> Ids = Room.RosterIds();
	Body->SetStringField(PlayServRoomsWire::FieldRosterHash, PlayServRosterHash::Compute(Ids));
	Body->SetNumberField(PlayServRoomsWire::FieldRosterCount, PlayServRosterHash::Count(Ids));
	return Body;
}

TSharedPtr<FJsonObject> PlayServRoomRuntime::BuildPresenceDelta(const FString& RoomName, int64 Seq, const TCHAR* Event, const FString& PlayerId, const FString& ReservationToken)
{
	TSharedPtr<FJsonObject> Frame = MakeShared<FJsonObject>();
	Frame->SetStringField(PlayServRoomsWire::FieldType, PlayServRoomsWire::TypeRoomPresence);
	Frame->SetStringField(PlayServRoomsWire::FieldRoomName, RoomName);
	Frame->SetNumberField(PlayServRoomsWire::FieldSeq, static_cast<double>(Seq));
	Frame->SetStringField(PlayServRoomsWire::FieldEvent, Event);
	Frame->SetStringField(PlayServRoomsWire::FieldPlayerId, PlayerId);
	Frame->SetField(PlayServRoomsWire::FieldConnectionId, MakeShared<FJsonValueNull>());
	if (!ReservationToken.IsEmpty())
	{
		Frame->SetStringField(PlayServRoomsWire::FieldReservationToken, ReservationToken);
	}
	return Frame;
}

TSharedPtr<FJsonObject> PlayServRoomRuntime::BuildRosterFrame(const FString& RoomName, int64 Seq, const TArray<FString>& PlayerIds)
{
	TSharedPtr<FJsonObject> Frame = MakeShared<FJsonObject>();
	Frame->SetStringField(PlayServRoomsWire::FieldType, PlayServRoomsWire::TypeRoomPresence);
	Frame->SetStringField(PlayServRoomsWire::FieldRoomName, RoomName);
	Frame->SetNumberField(PlayServRoomsWire::FieldSeq, static_cast<double>(Seq));
	Frame->SetStringField(PlayServRoomsWire::FieldEvent, PlayServRoomsWire::EventRoster);
	TArray<TSharedPtr<FJsonValue>> Members;
	for (const FString& PlayerId : PlayerIds)
	{
		TSharedPtr<FJsonObject> Member = MakeShared<FJsonObject>();
		Member->SetStringField(PlayServRoomsWire::FieldPlayerId, PlayerId);
		Member->SetField(PlayServRoomsWire::FieldConnectionId, MakeShared<FJsonValueNull>());
		Members.Add(MakeShared<FJsonValueObject>(Member));
	}
	Frame->SetArrayField(PlayServRoomsWire::FieldPlayers, Members);
	return Frame;
}

int64 PlayServRoomRuntime::UnixMillisecondsNow()
{
	const FTimespan SinceEpoch = FDateTime::UtcNow() - FDateTime(1970, 1, 1);
	return static_cast<int64>(SinceEpoch.GetTotalMilliseconds());
}
