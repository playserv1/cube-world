#pragma once

#include "CoreMinimal.h"

namespace PlayServRoomsWire
{
	static const TCHAR* TypeHello = TEXT("uplink_hello");
	static const TCHAR* TypePong = TEXT("pong");
	static const TCHAR* TypeRoomPresence = TEXT("room_presence");
	static const TCHAR* TypeTicketResult = TEXT("ticket_result");
	static const TCHAR* TypeTicketRelease = TEXT("ticket_release");
	static const TCHAR* TypeSubscribeData = TEXT("subscribe_data");
	static const TCHAR* TypeUnsubscribeData = TEXT("unsubscribe_data");
	static const TCHAR* TypeDataWrite = TEXT("data_write");

	static const TCHAR* TypeHelloAck = TEXT("uplink_hello_ack");
	static const TCHAR* TypePing = TEXT("ping");
	static const TCHAR* TypeSessionToken = TEXT("session_token");
	static const TCHAR* TypeTicketOffer = TEXT("ticket_offer");
	static const TCHAR* TypeJoinAck = TEXT("join_ack");
	static const TCHAR* TypeFrameTooLarge = TEXT("frame_too_large");
	static const TCHAR* TypeDataUpdate = TEXT("data_update");

	static const TCHAR* FieldType = TEXT("type");
	static const TCHAR* FieldExecutorSlug = TEXT("executor_slug");
	static const TCHAR* FieldInstanceId = TEXT("instance_id");
	static const TCHAR* FieldProtocolVersion = TEXT("protocol_version");
	static const TCHAR* FieldCapabilities = TEXT("capabilities");
	static const TCHAR* FieldResume = TEXT("resume");
	static const TCHAR* FieldSessionToken = TEXT("session_token");
	static const TCHAR* FieldExpiresIn = TEXT("expires_in");
	static const TCHAR* FieldAdmission = TEXT("admission");
	static const TCHAR* FieldRoomConfig = TEXT("room_config");
	static const TCHAR* FieldRoomName = TEXT("room_name");
	static const TCHAR* FieldSeq = TEXT("seq");
	static const TCHAR* FieldEvent = TEXT("event");
	static const TCHAR* FieldPlayerId = TEXT("player_id");
	static const TCHAR* FieldConnectionId = TEXT("connection_id");
	static const TCHAR* FieldPlayers = TEXT("players");
	static const TCHAR* FieldReservationToken = TEXT("reservation_token");
	static const TCHAR* FieldOk = TEXT("ok");
	static const TCHAR* FieldReason = TEXT("reason");
	static const TCHAR* FieldDetail = TEXT("detail");
	static const TCHAR* FieldParams = TEXT("params");

	// Data subscriptions and writes, as the C# SDK's RuntimeData speaks them. project_id and client_key are empty: the
	// platform scopes them to the project and environment the uplink signed in to.
	static const TCHAR* FieldProjectId = TEXT("project_id");
	static const TCHAR* FieldClientKey = TEXT("client_key");
	static const TCHAR* FieldEntity = TEXT("entity");
	static const TCHAR* FieldKeyPath = TEXT("key_path");
	static const TCHAR* FieldId = TEXT("id");
	static const TCHAR* FieldOp = TEXT("op");
	static const TCHAR* FieldData = TEXT("data");
	static const TCHAR* OpUpsert = TEXT("upsert");
	static const TCHAR* OpDelete = TEXT("delete");

	// A line in the game server's function logs, as the C# SDK's Platform.Log sends it (UplinkLog): message, level
	// (debug, info, warn, error) and optional data.
	static const TCHAR* TypeLog = TEXT("log");
	static const TCHAR* FieldMessage = TEXT("message");
	static const TCHAR* FieldLevel = TEXT("level");

	static const TCHAR* FieldCapacity = TEXT("capacity");
	static const TCHAR* FieldReservationTtlSeconds = TEXT("reservation_ttl_seconds");
	static const TCHAR* FieldRoomLifetimeSeconds = TEXT("room_lifetime_seconds");
	static const TCHAR* FieldRoomIdleTimeoutSeconds = TEXT("room_idle_timeout_seconds");
	static const TCHAR* FieldMaxRooms = TEXT("max_rooms");
	static const TCHAR* FieldVersion = TEXT("version");

	static const TCHAR* FieldPlayersCount = TEXT("players");
	static const TCHAR* FieldState = TEXT("state");
	static const TCHAR* FieldOpen = TEXT("open");
	static const TCHAR* FieldAttributes = TEXT("attributes");
	static const TCHAR* FieldConnect = TEXT("connect");
	static const TCHAR* FieldHost = TEXT("host");
	static const TCHAR* FieldPort = TEXT("port");
	static const TCHAR* FieldTransport = TEXT("transport");
	static const TCHAR* FieldConnectString = TEXT("connect_string");
	static const TCHAR* FieldRegion = TEXT("region");
	static const TCHAR* FieldRosterHash = TEXT("roster_hash");
	static const TCHAR* FieldRosterCount = TEXT("roster_count");

	static const TCHAR* FieldCreated = TEXT("created");
	static const TCHAR* FieldPlacement = TEXT("placement");
	static const TCHAR* FieldRosterCheck = TEXT("roster_check");

	static const TCHAR* EventJoin = TEXT("join");
	static const TCHAR* EventLeave = TEXT("leave");
	static const TCHAR* EventRoster = TEXT("roster");
	static const TCHAR* AdmissionPush = TEXT("push");
	static const TCHAR* AdmissionConsume = TEXT("consume");
	static const TCHAR* CapabilityAdmissionPush = TEXT("admission_push");
	static const TCHAR* CapabilityRoomCreate = TEXT("room_create");
	static const TCHAR* RosterCheckMismatch = TEXT("mismatch");
	static constexpr int32 ProtocolVersion = 1;

	static const TCHAR* ReasonReservationExpired = TEXT("reservation_expired");
	static const TCHAR* ReasonReservationConsumed = TEXT("reservation_consumed");
	static const TCHAR* ReasonReservationInvalid = TEXT("reservation_invalid");
	static const TCHAR* ReasonRoomMismatch = TEXT("room_mismatch");
	static const TCHAR* ReasonRoomClosed = TEXT("room_closed");
	static const TCHAR* ReasonRoomRefused = TEXT("room_refused");
	static const TCHAR* ReasonAdmissionUnavailable = TEXT("admission_unavailable");

	static const TCHAR* CloseHandshakeTimeout = TEXT("handshake_timeout");
	static const TCHAR* CloseProtocolUnsupported = TEXT("protocol_unsupported");
	static const TCHAR* CloseInstanceIdMissing = TEXT("instance_id_missing");
	static const TCHAR* CloseExecutorNotFound = TEXT("executor_not_found");
	static const TCHAR* CloseInstanceIdConflict = TEXT("instance_id_conflict");

	static constexpr int32 MaxFrameBytes = 1024 * 1024;
	static constexpr double HandshakeDeadlineSeconds = 10.0;
	static constexpr double PingIntervalSeconds = 5.0;
	static constexpr double DeadSocketSeconds = 15.0;
	static constexpr double HeartbeatIntervalSeconds = 5.0;
	static constexpr int32 InstanceConflictHelloThreshold = 3;
	static constexpr double InstanceConflictWindowSeconds = 10.0;
	static constexpr int32 MaxDetailLength = 128;
}
