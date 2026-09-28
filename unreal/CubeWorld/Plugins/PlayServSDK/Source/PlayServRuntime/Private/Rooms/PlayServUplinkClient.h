#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Rooms/PlayServRoomsTypes.h"
#include "Rooms/PlayServUplinkTransport.h"

struct FPlayServUplinkHelloParams
{
	FString ExecutorSlug;
	FString InstanceId;
	TArray<FString> Capabilities;
	bool bResume = false;
};

struct PLAYSERVRUNTIME_API FPlayServUplinkAck
{
	FString SessionToken;
	int32 ExpiresInSeconds = 0;
	FString Admission;
	bool bHasRoomConfig = false;
	FPlayServRoomConfig RoomConfig;

	static bool Parse(const TSharedPtr<FJsonObject>& Frame, FPlayServUplinkAck& OutAck, FString& OutError);
	static bool ParseRoomConfig(const TSharedPtr<FJsonObject>& Object, FPlayServRoomConfig& OutConfig);
};

class PLAYSERVRUNTIME_API FPlayServUplinkClient : public TSharedFromThis<FPlayServUplinkClient>
{
public:
	DECLARE_DELEGATE_OneParam(FOnUplinkStateChanged, EPlayServUplinkState);
	DECLARE_DELEGATE_TwoParams(FOnUplinkReady, const FPlayServUplinkAck&, int32);
	DECLARE_DELEGATE_TwoParams(FOnUplinkFrame, const FString&, const TSharedPtr<FJsonObject>&);
	DECLARE_DELEGATE_TwoParams(FOnUplinkRefused, const FString&, bool);

	FOnUplinkStateChanged OnStateChanged;
	FOnUplinkReady OnReady;
	FOnUplinkFrame OnFrame;
	FOnUplinkRefused OnRefused;

	FPlayServUplinkClient(FPlayServUplinkTransportFactory InTransportFactory, TFunction<double()> InClock);
	~FPlayServUplinkClient();

	void Start(const FString& Url, const FString& Credential, const FPlayServUplinkHelloParams& Hello);

	void Stop();

	bool SendFrame(const TSharedPtr<FJsonObject>& Frame);

	void Tick(double Now);

	EPlayServUplinkState GetState() const { return State; }
	const FPlayServUplinkAck& GetAck() const { return Ack; }
	int32 GetGeneration() const { return Generation; }
	bool IsStopped() const { return bStopped; }
	int32 GetConnectAttempts() const { return ConnectAttempts; }

	static TSharedPtr<FJsonObject> BuildHello(const FPlayServUplinkHelloParams& Hello);

	static FString SerializeFrame(const TSharedPtr<FJsonObject>& Frame);

	static double ReconnectDelaySeconds(int32 Attempt, float Jitter01);

	static bool IsPermanentRefusal(const FString& Reason);

	static double RenewalMarginSeconds(int32 ExpiresInSeconds);

private:
	void OpenSocket();
	void SetState(EPlayServUplinkState NewState);
	void ScheduleReconnect(double Now, double MinimumDelay);
	void DropSocket(int32 Code, const FString& Reason);
	void HandleConnected();
	void HandleConnectionError(const FString& Error);
	void HandleClosed(int32 StatusCode, const FString& Reason, bool bWasClean);
	void HandleMessage(const FString& Message);
	void SendHello();

	FPlayServUplinkTransportFactory TransportFactory;
	TFunction<double()> Clock;
	TSharedPtr<FPlayServUplinkTransport> Transport;

	FString Url;
	FString Credential;
	FPlayServUplinkHelloParams HelloParams;
	FPlayServUplinkAck Ack;

	EPlayServUplinkState State = EPlayServUplinkState::Disconnected;
	bool bStarted = false;
	bool bStopped = false;
	int32 Generation = 0;
	int32 ConnectAttempts = 0;
	int32 ReconnectAttempt = 0;
	double NextConnectAt = 0.0;
	double HelloSentAt = 0.0;
	double LastInboundAt = 0.0;
	double TokenExpiresAt = 0.0;
	bool bRenewalPending = false;
};
