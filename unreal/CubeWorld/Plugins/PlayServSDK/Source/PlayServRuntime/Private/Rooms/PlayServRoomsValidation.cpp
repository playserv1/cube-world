#include "Rooms/PlayServRoomsValidation.h"
#include "Dom/JsonObject.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	bool IsAsciiAlphaNumeric(TCHAR Character)
	{
		return (Character >= TEXT('a') && Character <= TEXT('z'))
			|| (Character >= TEXT('A') && Character <= TEXT('Z'))
			|| (Character >= TEXT('0') && Character <= TEXT('9'));
	}

	bool MatchesIdentifierGrammar(const FString& Value, int32 MaxLength)
	{
		if (Value.IsEmpty() || Value.Len() > MaxLength)
		{
			return false;
		}
		if (!IsAsciiAlphaNumeric(Value[0]))
		{
			return false;
		}
		for (int32 Index = 1; Index < Value.Len(); ++Index)
		{
			const TCHAR Character = Value[Index];
			if (!IsAsciiAlphaNumeric(Character) && Character != TEXT(':') && Character != TEXT('.')
				&& Character != TEXT('_') && Character != TEXT('-'))
			{
				return false;
			}
		}
		return true;
	}
}

bool PlayServRoomsValidation::IsValidRoomName(const FString& RoomName)
{
	return MatchesIdentifierGrammar(RoomName, MaxRoomNameLength);
}

bool PlayServRoomsValidation::IsValidInstanceId(const FString& InstanceId)
{
	return MatchesIdentifierGrammar(InstanceId, MaxInstanceIdLength);
}

bool PlayServRoomsValidation::IsValidRegion(const FString& Region)
{
	return Region.Len() <= MaxRegionLength;
}

int32 PlayServRoomsValidation::AttributesByteSize(const TMap<FString, FString>& Attributes)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	for (const TPair<FString, FString>& Pair : Attributes)
	{
		Object->SetStringField(Pair.Key, Pair.Value);
	}
	FString Serialized;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Serialized);
	FJsonSerializer::Serialize(Object, Writer);
	const FTCHARToUTF8 Converter(*Serialized);
	return Converter.Length();
}

int32 PlayServRoomsValidation::ParseCapacityAttribute(const FString& Text)
{
	if (Text.IsEmpty() || Text.Len() > 4)
	{
		return 0;
	}
	for (const TCHAR Character : Text)
	{
		if (!FChar::IsDigit(Character))
		{
			return 0;
		}
	}
	const int32 Seats = FCString::Atoi(*Text);
	return (Seats >= 1 && Seats <= MaxCapacityAttribute) ? Seats : 0;
}

bool PlayServRoomsValidation::IsValidConnect(const FPlayServRoomConnect& Connect, FString& OutReason)
{
	if (Connect.Host.TrimStartAndEnd().IsEmpty())
	{
		OutReason = TEXT("connect.host is empty");
		return false;
	}
	if (Connect.Port < 1 || Connect.Port > 65535)
	{
		OutReason = FString::Printf(TEXT("connect.port %d is outside 1..65535"), Connect.Port);
		return false;
	}
	if (Connect.ConnectString.Len() > MaxConnectStringLength)
	{
		OutReason = FString::Printf(TEXT("connect.connect_string exceeds %d characters"), MaxConnectStringLength);
		return false;
	}
	if (!IsValidRegion(Connect.Region))
	{
		OutReason = FString::Printf(TEXT("connect.region exceeds %d characters"), MaxRegionLength);
		return false;
	}
	return true;
}

FPlayServError PlayServRoomsValidation::ValidateSnapshot(const FPlayServRoomSnapshot& Snapshot)
{
	if (!IsValidRoomName(Snapshot.RoomName))
	{
		return FPlayServError::Make(EPlayServErrorCode::ValidationFailed,
			FString::Printf(TEXT("room_name '%s' must match ^[A-Za-z0-9][A-Za-z0-9:._-]{0,63}$ (a display name belongs in Attributes)"), *Snapshot.RoomName));
	}
	if (Snapshot.Capacity < 0)
	{
		return FPlayServError::Make(EPlayServErrorCode::ValidationFailed, TEXT("capacity must not be negative"));
	}
	if (!IsValidRegion(Snapshot.Region))
	{
		return FPlayServError::Make(EPlayServErrorCode::ValidationFailed,
			FString::Printf(TEXT("region exceeds %d characters"), MaxRegionLength));
	}
	const int32 AttributesBytes = AttributesByteSize(Snapshot.Attributes);
	if (AttributesBytes > MaxAttributesBytes)
	{
		return FPlayServError::Make(EPlayServErrorCode::ValidationFailed,
			FString::Printf(TEXT("attributes serialize to %d bytes; the platform accepts at most %d (attributes_too_large)"), AttributesBytes, MaxAttributesBytes));
	}
	if (Snapshot.Connect.IsSet() || !Snapshot.Connect.Host.IsEmpty())
	{
		FString Reason;
		if (!IsValidConnect(Snapshot.Connect, Reason))
		{
			return FPlayServError::Make(EPlayServErrorCode::ValidationFailed, Reason);
		}
	}
	return FPlayServError::Success();
}

const TCHAR* PlayServRoomsValidation::TransportToWire(EPlayServRoomTransport Transport)
{
	switch (Transport)
	{
	case EPlayServRoomTransport::Tcp: return TEXT("tcp");
	case EPlayServRoomTransport::Ws:  return TEXT("ws");
	case EPlayServRoomTransport::Wss: return TEXT("wss");
	case EPlayServRoomTransport::Udp:
	default:                          return TEXT("udp");
	}
}

bool PlayServRoomsValidation::TransportFromWire(const FString& Wire, EPlayServRoomTransport& OutTransport)
{
	const FString Lower = Wire.ToLower();
	if (Lower == TEXT("udp"))
	{
		OutTransport = EPlayServRoomTransport::Udp;
		return true;
	}
	if (Lower == TEXT("tcp"))
	{
		OutTransport = EPlayServRoomTransport::Tcp;
		return true;
	}
	if (Lower == TEXT("ws"))
	{
		OutTransport = EPlayServRoomTransport::Ws;
		return true;
	}
	if (Lower == TEXT("wss"))
	{
		OutTransport = EPlayServRoomTransport::Wss;
		return true;
	}
	return false;
}
