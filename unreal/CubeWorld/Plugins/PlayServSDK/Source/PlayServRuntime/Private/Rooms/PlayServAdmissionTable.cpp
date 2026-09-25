#include "Rooms/PlayServAdmissionTable.h"
#include "Rooms/PlayServRoomsWire.h"

void FPlayServAdmissionTable::Add(const FPlayServPushedTicket& Ticket)
{
	if (Ticket.ReservationToken.IsEmpty())
	{
		return;
	}
	Tickets.Add(Ticket.ReservationToken, Ticket);
}

bool FPlayServAdmissionTable::TryRedeem(const FString& ReservationToken, double Now, const FString& ExpectedRoom, FPlayServPushedTicket& OutTicket, FString& OutReason)
{
	FPlayServPushedTicket* Ticket = Tickets.Find(ReservationToken);
	if (Ticket == nullptr)
	{
		OutReason = PlayServRoomsWire::ReasonReservationInvalid;
		return false;
	}
	if (Ticket->bRoomClosed)
	{
		OutReason = PlayServRoomsWire::ReasonRoomClosed;
		return false;
	}
	if (Ticket->bRedeemed)
	{
		OutReason = PlayServRoomsWire::ReasonReservationConsumed;
		return false;
	}
	if (Now >= Ticket->ExpiresAt)
	{
		OutReason = PlayServRoomsWire::ReasonReservationExpired;
		return false;
	}
	if (!ExpectedRoom.IsEmpty() && Ticket->RoomName != ExpectedRoom)
	{
		OutReason = PlayServRoomsWire::ReasonRoomMismatch;
		return false;
	}
	Ticket->bRedeemed = true;
	OutTicket = *Ticket;
	OutReason.Empty();
	return true;
}

bool FPlayServAdmissionTable::Contains(const FString& ReservationToken) const
{
	return Tickets.Contains(ReservationToken);
}

bool FPlayServAdmissionTable::Remove(const FString& ReservationToken)
{
	return Tickets.Remove(ReservationToken) > 0;
}

void FPlayServAdmissionTable::RemoveRoom(const FString& RoomName, TArray<FPlayServPushedTicket>& OutUnredeemed)
{
	for (TMap<FString, FPlayServPushedTicket>::TIterator It(Tickets); It; ++It)
	{
		if (It->Value.RoomName == RoomName)
		{
			if (!It->Value.bRedeemed)
			{
				OutUnredeemed.Add(It->Value);
			}
			It->Value.bRoomClosed = true;
		}
	}
}

void FPlayServAdmissionTable::SweepExpired(double Now, TArray<FPlayServPushedTicket>& OutExpiredUnredeemed)
{
	for (TMap<FString, FPlayServPushedTicket>::TIterator It(Tickets); It; ++It)
	{
		const FPlayServPushedTicket& Ticket = It->Value;
		if (!Ticket.bRedeemed && !Ticket.bRoomClosed && Now >= Ticket.ExpiresAt)
		{
			OutExpiredUnredeemed.Add(Ticket);
			It.RemoveCurrent();
		}
		else if ((Ticket.bRedeemed || Ticket.bRoomClosed) && Now >= Ticket.ExpiresAt + RedeemedRetentionSeconds)
		{
			It.RemoveCurrent();
		}
	}
}

FString FPlayServAdmissionTable::ParseTravelOption(const FString& Options, const FString& Key)
{
	if (Key.IsEmpty())
	{
		return FString();
	}
	TArray<FString> Segments;
	Options.ParseIntoArray(Segments, TEXT("?"), true);
	for (const FString& Segment : Segments)
	{
		int32 EqualsIndex = INDEX_NONE;
		if (!Segment.FindChar(TEXT('='), EqualsIndex))
		{
			continue;
		}
		if (Segment.Left(EqualsIndex).Equals(Key, ESearchCase::IgnoreCase))
		{
			return Segment.Mid(EqualsIndex + 1);
		}
	}
	return FString();
}
