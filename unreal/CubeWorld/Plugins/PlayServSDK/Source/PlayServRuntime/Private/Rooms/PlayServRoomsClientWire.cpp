#include "Rooms/PlayServRoomsClientWire.h"
#include "Rooms/PlayServRoomsValidation.h"
#include "Dom/JsonValue.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "Misc/DateTime.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	const TCHAR* ProblemRoomUnreachable = TEXT("room_unreachable");

	FString ValueToString(const TSharedPtr<FJsonValue>& Value)
	{
		if (!Value.IsValid())
		{
			return FString();
		}
		FString Text;
		if (Value->TryGetString(Text))
		{
			return Text;
		}
		bool bBool = false;
		if (Value->TryGetBool(bBool))
		{
			return bBool ? TEXT("true") : TEXT("false");
		}
		double Number = 0.0;
		if (Value->TryGetNumber(Number))
		{
			return FString::SanitizeFloat(Number, 0);
		}
		FString Serialized;
		TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Serialized);
		if (FJsonSerializer::Serialize(Value, FString(), Writer))
		{
			return Serialized;
		}
		return FString();
	}

	void AddAttributesOf(const TSharedPtr<FJsonObject>& Object, TMap<FString, FString>& OutAttributes)
	{
		for (const auto& Pair : Object->Values)
		{
			OutAttributes.Add(FString(*Pair.Key), ValueToString(Pair.Value));
		}
	}

	FPlayServError RoomNeverPublishedAddress(float WaitedSeconds)
	{
		FPlayServError Error = FPlayServError::Make(EPlayServErrorCode::NotFound,
			FString::Printf(TEXT("room_unreachable: the matched room's server did not publish an address within %.0f s"), WaitedSeconds));
		Error.ProblemCode = ProblemRoomUnreachable;
		return Error;
	}
}

const TCHAR* PlayServRoomsClientWire::PlacementStateToWire(EPlayServPlacementState State)
{
	switch (State)
	{
	case EPlayServPlacementState::Open:           return TEXT("open");
	case EPlayServPlacementState::Full:           return TEXT("full");
	case EPlayServPlacementState::SelfClosed:     return TEXT("self_closed");
	case EPlayServPlacementState::Draining:       return TEXT("draining");
	case EPlayServPlacementState::SessionClosing: return TEXT("session_closing");
	default:                                      return TEXT("");
	}
}

EPlayServPlacementState PlayServRoomsClientWire::PlacementStateFromWire(const FString& Wire)
{
	if (Wire == TEXT("open"))            return EPlayServPlacementState::Open;
	if (Wire == TEXT("full"))            return EPlayServPlacementState::Full;
	if (Wire == TEXT("self_closed"))     return EPlayServPlacementState::SelfClosed;
	if (Wire == TEXT("draining"))        return EPlayServPlacementState::Draining;
	if (Wire == TEXT("session_closing")) return EPlayServPlacementState::SessionClosing;
	return EPlayServPlacementState::Unknown;
}

FString PlayServRoomsClientWire::BuildBrowseQuery(const FPlayServRoomFilters& Filters)
{
	TArray<FString> Pairs;
	if (Filters.PlacementState.IsSet())
	{
		const FString Wire = PlacementStateToWire(Filters.PlacementState.GetValue());
		if (!Wire.IsEmpty())
		{
			Pairs.Add(FString::Printf(TEXT("placement_state=%s"), *Wire));
		}
	}
	if (!Filters.Region.IsEmpty())
	{
		Pairs.Add(FString::Printf(TEXT("region=%s"), *FGenericPlatformHttp::UrlEncode(Filters.Region)));
	}

	TArray<FString> AttributeKeys;
	Filters.Attributes.GetKeys(AttributeKeys);
	AttributeKeys.Sort();
	for (const FString& Key : AttributeKeys)
	{
		Pairs.Add(FString::Printf(TEXT("attributes.%s=%s"),
			*FGenericPlatformHttp::UrlEncode(Key), *FGenericPlatformHttp::UrlEncode(Filters.Attributes[Key])));
	}

	if (!Filters.Cursor.IsEmpty())
	{
		Pairs.Add(FString::Printf(TEXT("cursor=%s"), *FGenericPlatformHttp::UrlEncode(Filters.Cursor)));
	}
	return Pairs.Num() == 0 ? FString() : FString::Printf(TEXT("?%s"), *FString::Join(Pairs, TEXT("&")));
}

bool PlayServRoomsClientWire::ParseConnect(const TSharedPtr<FJsonObject>& Json, const FString& Field, FPlayServRoomConnect& OutConnect)
{
	const TSharedPtr<FJsonObject>* Object = nullptr;
	if (!Json.IsValid() || !Json->TryGetObjectField(Field, Object) || Object == nullptr || !Object->IsValid())
	{
		return false;
	}
	(*Object)->TryGetStringField(TEXT("host"), OutConnect.Host);
	(*Object)->TryGetNumberField(TEXT("port"), OutConnect.Port);
	FString Transport;
	if ((*Object)->TryGetStringField(TEXT("transport"), Transport))
	{
		PlayServRoomsValidation::TransportFromWire(Transport, OutConnect.Transport);
	}
	(*Object)->TryGetStringField(TEXT("connect_string"), OutConnect.ConnectString);
	(*Object)->TryGetStringField(TEXT("region"), OutConnect.Region);
	return OutConnect.IsSet();
}

void PlayServRoomsClientWire::ParseAttributes(const TSharedPtr<FJsonObject>& Json, const FString& Field, TMap<FString, FString>& OutAttributes)
{
	const TSharedPtr<FJsonObject>* Object = nullptr;
	if (!Json.IsValid() || !Json->TryGetObjectField(Field, Object) || Object == nullptr || !Object->IsValid())
	{
		return;
	}
	AddAttributesOf(*Object, OutAttributes);
}

bool PlayServRoomsClientWire::ParseAttributesJson(const FString& Text, TMap<FString, FString>& OutAttributes)
{
	TSharedPtr<FJsonObject> Object;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (Text.IsEmpty() || !FJsonSerializer::Deserialize(Reader, Object) || !Object.IsValid())
	{
		return false;
	}
	AddAttributesOf(Object, OutAttributes);
	return true;
}

TSharedPtr<FJsonObject> PlayServRoomsClientWire::BuildHostBody(const TMap<FString, FString>& Attributes)
{
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	if (Attributes.Num() > 0)
	{
		TSharedPtr<FJsonObject> Object = MakeShared<FJsonObject>();
		for (const TPair<FString, FString>& Attribute : Attributes)
		{
			Object->SetStringField(Attribute.Key, Attribute.Value);
		}
		Body->SetObjectField(TEXT("attributes"), Object);
	}
	return Body;
}

bool PlayServRoomsClientWire::ParseHostedRoom(const TSharedPtr<FJsonObject>& Json, FPlayServRoomListing& OutRoom)
{
	if (!Json.IsValid() || !Json->TryGetStringField(TEXT("room_name"), OutRoom.RoomName) || OutRoom.RoomName.IsEmpty())
	{
		return false;
	}
	Json->TryGetStringField(TEXT("region"), OutRoom.Region);
	ParseAttributes(Json, TEXT("attributes"), OutRoom.Attributes);
	ParseConnect(Json, TEXT("connect"), OutRoom.Connect);
	OutRoom.Players = 0;
	return true;
}

bool PlayServRoomsClientWire::ParseBrowsePage(const TSharedPtr<FJsonObject>& Json, FPlayServBrowsePage& OutPage)
{
	const TArray<TSharedPtr<FJsonValue>>* Data = nullptr;
	if (!Json.IsValid() || !Json->TryGetArrayField(TEXT("data"), Data) || Data == nullptr)
	{
		return false;
	}

	for (const TSharedPtr<FJsonValue>& Element : *Data)
	{
		const TSharedPtr<FJsonObject>* Item = nullptr;
		if (!Element.IsValid() || !Element->TryGetObject(Item) || Item == nullptr)
		{
			continue;
		}
		FPlayServRoomListing Listing;
		(*Item)->TryGetStringField(TEXT("room_name"), Listing.RoomName);
		(*Item)->TryGetNumberField(TEXT("players"), Listing.Players);
		(*Item)->TryGetNumberField(TEXT("capacity"), Listing.Capacity);
		(*Item)->TryGetStringField(TEXT("state"), Listing.State);
		FString PlacementState;
		if ((*Item)->TryGetStringField(TEXT("placement_state"), PlacementState))
		{
			Listing.PlacementState = PlacementStateFromWire(PlacementState);
		}
		(*Item)->TryGetStringField(TEXT("region"), Listing.Region);
		ParseAttributes(*Item, TEXT("attributes"), Listing.Attributes);
		ParseConnect(*Item, TEXT("connect"), Listing.Connect);
		OutPage.Rooms.Add(MoveTemp(Listing));
	}

	const TSharedPtr<FJsonObject>* Page = nullptr;
	if (Json->TryGetObjectField(TEXT("page"), Page) && Page != nullptr && Page->IsValid())
	{
		(*Page)->TryGetStringField(TEXT("cursor_next"), OutPage.NextCursor);
		(*Page)->TryGetBoolField(TEXT("has_more"), OutPage.bHasMore);
	}
	return true;
}

bool PlayServRoomsClientWire::ParseMatched(const TSharedPtr<FJsonObject>& Json, const FString& DateHeader, double ReceivedAtMonotonic, FPlayServRoomTicket& OutTicket)
{
	if (!Json.IsValid() || !Json->TryGetStringField(TEXT("reservation_token"), OutTicket.ReservationToken)
		|| OutTicket.ReservationToken.IsEmpty())
	{
		return false;
	}
	Json->TryGetStringField(TEXT("room_name"), OutTicket.RoomName);
	Json->TryGetStringField(TEXT("region"), OutTicket.Region);
	ParseAttributes(Json, TEXT("attributes"), OutTicket.Attributes);
	ParseConnect(Json, TEXT("connect"), OutTicket.Connect);

	int32 ExpiresIn = 0;
	if (Json->TryGetNumberField(TEXT("expires_in"), ExpiresIn) && ExpiresIn >= 0)
	{
		OutTicket.ExpiresInSeconds = ExpiresIn;
	}
	else
	{
		FString ExpiresAtText;
		FDateTime ExpiresAt;
		FDateTime ServedAt;
		if (Json->TryGetStringField(TEXT("expires_at"), ExpiresAtText)
			&& FDateTime::ParseIso8601(*ExpiresAtText, ExpiresAt)
			&& !DateHeader.IsEmpty()
			&& FDateTime::ParseHttpDate(DateHeader, ServedAt))
		{
			OutTicket.ExpiresInSeconds = FMath::Max(0, static_cast<int32>((ExpiresAt - ServedAt).GetTotalSeconds()));
		}
	}
	OutTicket.ExpiresAtMonotonic = ReceivedAtMonotonic + static_cast<double>(OutTicket.ExpiresInSeconds);
	return true;
}

FString PlayServRoomsClientWire::BuildTravelUrl(const FPlayServRoomTicket& Ticket)
{
	const FString Address = Ticket.Connect.ConnectString.IsEmpty()
		? (Ticket.Connect.IsSet() ? FString::Printf(TEXT("%s:%d"), *Ticket.Connect.Host, Ticket.Connect.Port) : FString())
		: Ticket.Connect.ConnectString;
	if (Address.IsEmpty())
	{
		return FString();
	}
	return FString::Printf(TEXT("%s?rsv=%s"), *Address, *Ticket.ReservationToken);
}

PlayServRoomsClientWire::FJoinStepDecision PlayServRoomsClientWire::DecideAfterJoin(const FJoinLoopState& State, const FJoinAnswer& Answer)
{
	FJoinStepDecision Decision;
	const bool bBudgetSpent = State.ElapsedSeconds >= static_cast<double>(JoinTimeoutSeconds);

	if (Answer.bSuccess)
	{
		if (!Answer.bHasTicket)
		{
			Decision.Step = EJoinStep::Stop;
			Decision.Error = FPlayServError::Make(EPlayServErrorCode::ContractMismatch,
				TEXT("The platform answered `:join` without a reservation token"));
			return Decision;
		}
		if (Answer.bTicketHasConnect)
		{
			Decision.Step = EJoinStep::Deliver;
			return Decision;
		}
		if (State.ConnectWaitedSeconds >= static_cast<double>(ConnectWaitSeconds) || bBudgetSpent)
		{
			Decision.Step = EJoinStep::Stop;
			Decision.Error = RoomNeverPublishedAddress(ConnectWaitSeconds);
			return Decision;
		}
		Decision.Step = EJoinStep::JoinNamedRoom;
		Decision.DelaySeconds = ConnectPollIntervalSeconds;
		return Decision;
	}

	const bool bRetryable = Answer.ProblemCode == ProblemRoomUnreachable;

	if (!bRetryable || bBudgetSpent || State.Retries >= MaxRetries)
	{
		Decision.Step = EJoinStep::Stop;
		Decision.Error = Answer.Error;
		return Decision;
	}

	Decision.Step = EJoinStep::JoinNamedRoom;
	Decision.DelaySeconds = static_cast<double>(Answer.RetryAfterSeconds);
	Decision.bSpendsRetry = true;
	return Decision;
}
