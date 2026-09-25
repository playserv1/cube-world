#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

struct FPlayServPushedTicket
{
	FString ReservationToken;
	FString RoomName;
	FString PlayerId;
	double ExpiresAt = 0.0;
	bool bRedeemed = false;
	bool bRoomClosed = false;
	TSharedPtr<FJsonObject> Params;
};

class PLAYSERVRUNTIME_API FPlayServAdmissionTable
{
public:
	void Add(const FPlayServPushedTicket& Ticket);

	bool TryRedeem(const FString& ReservationToken, double Now, const FString& ExpectedRoom, FPlayServPushedTicket& OutTicket, FString& OutReason);

	bool Contains(const FString& ReservationToken) const;
	bool Remove(const FString& ReservationToken);

	void RemoveRoom(const FString& RoomName, TArray<FPlayServPushedTicket>& OutUnredeemed);

	void SweepExpired(double Now, TArray<FPlayServPushedTicket>& OutExpiredUnredeemed);

	int32 Num() const { return Tickets.Num(); }

	static FString ParseTravelOption(const FString& Options, const FString& Key);

private:
	static constexpr double RedeemedRetentionSeconds = 300.0;

	TMap<FString, FPlayServPushedTicket> Tickets;
};
