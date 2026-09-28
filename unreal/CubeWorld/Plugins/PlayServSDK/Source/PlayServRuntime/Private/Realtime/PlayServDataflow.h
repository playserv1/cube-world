#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"

class IWebSocket;

class FPlayServDataflow : public TSharedFromThis<FPlayServDataflow>
{
public:
	DECLARE_DELEGATE_ThreeParams(FOnDataflowUpdate, const FString&, const FString&, TSharedPtr<FJsonObject>);
	DECLARE_DELEGATE_ThreeParams(FOnDataflowTerminated, const FString&, const FString&, int32);
	DECLARE_DELEGATE(FOnDataflowConnectionLost);

	FOnDataflowUpdate OnUpdate;
	FOnDataflowTerminated OnTerminated;
	FOnDataflowConnectionLost OnConnectionLost;

	~FPlayServDataflow();

	void SubscribeRecord(const FString& EntityType, const FString& RecordId);

	void UnsubscribeRecord(const FString& EntityType, const FString& RecordId);

	void Shutdown();

#if !UE_BUILD_SHIPPING
	static PLAYSERVRUNTIME_API int32 TestTerminatedFrameCount;
	static PLAYSERVRUNTIME_API int32 TestLastTerminatedCode;
	static PLAYSERVRUNTIME_API int32 TestRegisteredRowCount;
	PLAYSERVRUNTIME_API void TestSimulatePlatformClose(int32 StatusCode);
#endif

private:
	struct FRowSubscription
	{
		FString EntityType;
		FString RecordId;
		int32 RefCount = 0;
		int64 WsSubscriptionId = 0;
		int64 RegisterRequestId = 0;
	};

	enum class EConnectionState : uint8 { Disconnected, Connecting, HandshakeSent, Ready };

	static FString MakeKey(const FString& EntityType, const FString& RecordId);

	void EnsureConnected();
	void SendHandshake();
	void OnSocketConnected();
	void OnSocketConnectionError(const FString& Error);
	void OnSocketMessage(const FString& Message);
	void OnSocketClosed(int32 StatusCode, const FString& Reason, bool bWasClean);
	void RegisterRow(FRowSubscription& Row);
	void CloseRowOnWire(const FRowSubscription& Row);
	void HandleSubscriptionResponse(const TSharedPtr<FJsonObject>& Payload);
	void HandleSubscriptionUpdate(const TSharedPtr<FJsonObject>& Payload);
	void SendEnvelope(const FString& Command, const TSharedPtr<FJsonObject>& Payload);
	bool TickMaintenance(float DeltaTime);
	void FailAllSubscriptions();

	FRowSubscription* FindRowBySubscriptionId(int64 SubscriptionId);
	FRowSubscription* FindRowByRequestId(int64 RequestId);

	TSharedPtr<IWebSocket> Socket;
	EConnectionState State = EConnectionState::Disconnected;
	bool bShuttingDown = false;
	bool bReconnectAttempted = false;

	TMap<FString, FRowSubscription> RowsByKey;
	int64 NextRequestId = 1;

	FString LastSentAccessToken;
	FTSTicker::FDelegateHandle MaintenanceTickerHandle;
};
