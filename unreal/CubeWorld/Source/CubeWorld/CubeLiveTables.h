// Live tables: the platform's collection subscriptions over its realtime socket (wss://<platform>/ws/). A
// subscription names an entity and a filter; the platform sends the whole matching set (at most 200 rows) once on
// subscribe and again after every change to a row of that table, so a server hears the other servers' writes the
// moment they land instead of polling for them. The dedicated server signs the socket's handshake with its server
// credential alone (the deployment token PlayServ hosting gives it, else the server key); the SDK's own realtime
// client sends the client key as well, which the platform refuses for a server, so this client speaks the wire
// itself. The frames are the SDK's (module_dataflow.*), so the platform sees one more SDK client.
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"

class IWebSocket;

class FCubeLiveTables : public TSharedFromThis<FCubeLiveTables>
{
public:
	/** The rows the platform sent: each a JSON object of the row's fields plus its `id`. */
	DECLARE_DELEGATE_OneParam(FOnLiveRows, const TArray<TSharedPtr<FJsonObject>>&);

	~FCubeLiveTables();

	/**
	 * Registers a query and opens the socket when it is not open. Query is the platform's dataflow query, such as
	 * `WorldCube(where: { at: { gt: 1790000000000 } }, limit: 200) { * }`; without `limit` a set over 200 rows is
	 * refused, with it the set is cut at the limit. Returns a handle for Unsubscribe.
	 */
	int32 Subscribe(const FString& EntityType, const FString& Query, FOnLiveRows OnRows);
	void Unsubscribe(int32 Handle);
	void Shutdown();
	bool IsReady() const { return State == EState::Ready; }

private:
	struct FSub
	{
		FString EntityType;
		FString Query;
		FOnLiveRows OnRows;
		int64 RequestId = 0;
		int64 SubscriptionId = 0;
		/** When a refused or terminated subscription is tried again. */
		double RetryAt = 0;
	};

	enum class EState : uint8 { Disconnected, Connecting, HandshakeSent, Ready };

	void Connect();
	void SendHandshake();
	void Register(int32 Handle, FSub& Sub);
	void SendEnvelope(const TCHAR* Command, const TSharedPtr<FJsonObject>& Payload);
	void OnSocketMessage(const FString& Message);
	void OnSocketClosed(int32 StatusCode, const FString& Reason);
	void OnSubscribeResponse(const TSharedPtr<FJsonObject>& Payload);
	void OnUpdate(const TSharedPtr<FJsonObject>& Payload);
	bool Tick(float DeltaTime);
	int32 FindBySubscriptionId(int64 SubscriptionId) const;
	int32 FindByRequestId(int64 RequestId) const;

	TSharedPtr<IWebSocket> Socket;
	EState State = EState::Disconnected;
	TMap<int32, FSub> Subs;
	int32 NextHandle = 1;
	int64 NextRequestId = 1;
	double ReconnectAt = 0;
	bool bShuttingDown = false;
	FTSTicker::FDelegateHandle TickerHandle;
};
