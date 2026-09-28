#include "Rooms/PlayServUplinkClient.h"
#include "Core/PlayServLog.h"
#include "Rooms/PlayServRoomsWire.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	constexpr double ReconnectBaseSeconds = 1.0;
	constexpr double ReconnectMaxSeconds = 30.0;
	constexpr double ReconnectJitterFraction = 0.2;
	constexpr int32 CloseCodeGoingAway = 1001;
	constexpr int32 CloseCodeProtocolError = 1002;
	constexpr int32 CloseCodePolicyViolation = 1008;
}

bool FPlayServUplinkAck::ParseRoomConfig(const TSharedPtr<FJsonObject>& Object, FPlayServRoomConfig& OutConfig)
{
	if (!Object.IsValid())
	{
		return false;
	}
	FPlayServRoomConfig Config;
	double Number = 0.0;
	if (Object->TryGetNumberField(PlayServRoomsWire::FieldCapacity, Number))
	{
		Config.Capacity = static_cast<int32>(Number);
	}
	if (Object->TryGetNumberField(PlayServRoomsWire::FieldReservationTtlSeconds, Number))
	{
		Config.ReservationTtlSeconds = static_cast<int32>(Number);
	}
	if (Object->TryGetNumberField(PlayServRoomsWire::FieldRoomLifetimeSeconds, Number))
	{
		Config.RoomLifetimeSeconds = static_cast<int32>(Number);
	}
	if (Object->TryGetNumberField(PlayServRoomsWire::FieldRoomIdleTimeoutSeconds, Number))
	{
		Config.bHasIdleTimeout = true;
		Config.RoomIdleTimeoutSeconds = static_cast<int32>(Number);
	}
	if (Object->TryGetNumberField(PlayServRoomsWire::FieldMaxRooms, Number))
	{
		Config.MaxRooms = static_cast<int32>(Number);
	}
	if (Object->TryGetNumberField(PlayServRoomsWire::FieldVersion, Number))
	{
		Config.Version = static_cast<int32>(Number);
	}
	OutConfig = Config;
	return true;
}

bool FPlayServUplinkAck::Parse(const TSharedPtr<FJsonObject>& Frame, FPlayServUplinkAck& OutAck, FString& OutError)
{
	if (!Frame.IsValid())
	{
		OutError = TEXT("hello ack is not a JSON object");
		return false;
	}
	FPlayServUplinkAck Parsed;
	if (!Frame->TryGetStringField(PlayServRoomsWire::FieldSessionToken, Parsed.SessionToken) || Parsed.SessionToken.IsEmpty())
	{
		OutError = TEXT("hello ack carries no session_token");
		return false;
	}
	double ExpiresIn = 0.0;
	Frame->TryGetNumberField(PlayServRoomsWire::FieldExpiresIn, ExpiresIn);
	Parsed.ExpiresInSeconds = static_cast<int32>(ExpiresIn);
	Frame->TryGetStringField(PlayServRoomsWire::FieldAdmission, Parsed.Admission);
	if (Parsed.Admission.IsEmpty())
	{
		Parsed.Admission = PlayServRoomsWire::AdmissionConsume;
	}
	const TSharedPtr<FJsonObject>* ConfigObject = nullptr;
	if (Frame->TryGetObjectField(PlayServRoomsWire::FieldRoomConfig, ConfigObject))
	{
		Parsed.bHasRoomConfig = ParseRoomConfig(*ConfigObject, Parsed.RoomConfig);
	}
	OutAck = Parsed;
	return true;
}

FPlayServUplinkClient::FPlayServUplinkClient(FPlayServUplinkTransportFactory InTransportFactory, TFunction<double()> InClock)
	: TransportFactory(MoveTemp(InTransportFactory))
	, Clock(MoveTemp(InClock))
{
}

FPlayServUplinkClient::~FPlayServUplinkClient()
{
	if (Transport.IsValid())
	{
		Transport->OnConnected.Unbind();
		Transport->OnConnectionError.Unbind();
		Transport->OnClosed.Unbind();
		Transport->OnMessage.Unbind();
		Transport->Close(CloseCodeGoingAway, TEXT("shutdown"));
		Transport.Reset();
	}
}

TSharedPtr<FJsonObject> FPlayServUplinkClient::BuildHello(const FPlayServUplinkHelloParams& Hello)
{
	TSharedPtr<FJsonObject> Frame = MakeShared<FJsonObject>();
	Frame->SetStringField(PlayServRoomsWire::FieldType, PlayServRoomsWire::TypeHello);
	if (!Hello.ExecutorSlug.IsEmpty())
	{
		Frame->SetStringField(PlayServRoomsWire::FieldExecutorSlug, Hello.ExecutorSlug);
	}
	Frame->SetStringField(PlayServRoomsWire::FieldInstanceId, Hello.InstanceId);
	Frame->SetNumberField(PlayServRoomsWire::FieldProtocolVersion, PlayServRoomsWire::ProtocolVersion);
	TArray<TSharedPtr<FJsonValue>> Capabilities;
	for (const FString& Capability : Hello.Capabilities)
	{
		Capabilities.Add(MakeShared<FJsonValueString>(Capability));
	}
	Frame->SetArrayField(PlayServRoomsWire::FieldCapabilities, Capabilities);
	if (Hello.bResume)
	{
		Frame->SetBoolField(PlayServRoomsWire::FieldResume, true);
	}
	return Frame;
}

FString FPlayServUplinkClient::SerializeFrame(const TSharedPtr<FJsonObject>& Frame)
{
	FString Text;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
	FJsonSerializer::Serialize(Frame.ToSharedRef(), Writer);
	return Text;
}

double FPlayServUplinkClient::ReconnectDelaySeconds(int32 Attempt, float Jitter01)
{
	const int32 Exponent = FMath::Clamp(Attempt, 0, 30);
	double Delay = ReconnectBaseSeconds * static_cast<double>(1u << FMath::Min(Exponent, 5));
	Delay = FMath::Min(Delay, ReconnectMaxSeconds);
	const double JitterScale = 1.0 + (static_cast<double>(FMath::Clamp(Jitter01, 0.0f, 1.0f)) * 2.0 - 1.0) * ReconnectJitterFraction;
	return Delay * JitterScale;
}

bool FPlayServUplinkClient::IsPermanentRefusal(const FString& Reason)
{
	return Reason == PlayServRoomsWire::CloseExecutorNotFound
		|| Reason == PlayServRoomsWire::CloseProtocolUnsupported
		|| Reason == PlayServRoomsWire::CloseInstanceIdMissing;
}

double FPlayServUplinkClient::RenewalMarginSeconds(int32 ExpiresInSeconds)
{
	const double Tenth = static_cast<double>(ExpiresInSeconds) * 0.1;
	return FMath::Clamp(FMath::Max(Tenth, 60.0), 1.0, static_cast<double>(ExpiresInSeconds) * 0.5);
}

void FPlayServUplinkClient::Start(const FString& InUrl, const FString& InCredential, const FPlayServUplinkHelloParams& InHello)
{
	Url = InUrl;
	Credential = InCredential;
	HelloParams = InHello;
	bStarted = true;
	bStopped = false;
	ReconnectAttempt = 0;
	NextConnectAt = 0.0;
	OpenSocket();
}

void FPlayServUplinkClient::Stop()
{
	bStopped = true;
	bStarted = false;
	bRenewalPending = false;
	DropSocket(CloseCodeGoingAway, TEXT("shutdown"));
	SetState(EPlayServUplinkState::Disconnected);
}

void FPlayServUplinkClient::OpenSocket()
{
	if (bStopped)
	{
		return;
	}
	DropSocket(CloseCodeGoingAway, TEXT("reconnect"));
	Transport = TransportFactory(Url, Credential);
	if (!Transport.IsValid())
	{
		UE_LOG(LogPlayServ, Error, TEXT("PlayServ uplink: no transport available"));
		return;
	}
	TWeakPtr<FPlayServUplinkClient> WeakSelf(AsShared());
	Transport->OnConnected.BindLambda([WeakSelf]()
	{
		if (TSharedPtr<FPlayServUplinkClient> Self = WeakSelf.Pin())
		{
			Self->HandleConnected();
		}
	});
	Transport->OnConnectionError.BindLambda([WeakSelf](const FString& Error)
	{
		if (TSharedPtr<FPlayServUplinkClient> Self = WeakSelf.Pin())
		{
			Self->HandleConnectionError(Error);
		}
	});
	Transport->OnClosed.BindLambda([WeakSelf](int32 StatusCode, const FString& Reason, bool bWasClean)
	{
		if (TSharedPtr<FPlayServUplinkClient> Self = WeakSelf.Pin())
		{
			Self->HandleClosed(StatusCode, Reason, bWasClean);
		}
	});
	Transport->OnMessage.BindLambda([WeakSelf](const FString& Message)
	{
		if (TSharedPtr<FPlayServUplinkClient> Self = WeakSelf.Pin())
		{
			Self->HandleMessage(Message);
		}
	});

	++ConnectAttempts;
	NextConnectAt = 0.0;
	LastInboundAt = Clock();
	SetState(EPlayServUplinkState::Connecting);
	Transport->Connect();
}

void FPlayServUplinkClient::DropSocket(int32 Code, const FString& Reason)
{
	if (!Transport.IsValid())
	{
		return;
	}
	TSharedPtr<FPlayServUplinkTransport> Old = Transport;
	Transport.Reset();
	Old->OnConnected.Unbind();
	Old->OnConnectionError.Unbind();
	Old->OnClosed.Unbind();
	Old->OnMessage.Unbind();
	Old->Close(Code, Reason);
}

void FPlayServUplinkClient::SetState(EPlayServUplinkState NewState)
{
	if (State == NewState)
	{
		return;
	}
	State = NewState;
	OnStateChanged.ExecuteIfBound(State);
}

void FPlayServUplinkClient::ScheduleReconnect(double Now, double MinimumDelay)
{
	if (bStopped)
	{
		return;
	}
	const double Delay = FMath::Max(MinimumDelay, ReconnectDelaySeconds(ReconnectAttempt, FMath::FRand()));
	++ReconnectAttempt;
	NextConnectAt = Now + Delay;
	SetState(EPlayServUplinkState::Disconnected);
	UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ uplink: reconnect in %.1fs (attempt %d)"), Delay, ReconnectAttempt);
}

void FPlayServUplinkClient::HandleConnected()
{
	SendHello();
}

void FPlayServUplinkClient::SendHello()
{
	if (!Transport.IsValid())
	{
		return;
	}
	const TSharedPtr<FJsonObject> Hello = BuildHello(HelloParams);
	UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ uplink: hello (executor=%s, instance=%s, capabilities=%d)"),
		*HelloParams.ExecutorSlug, *HelloParams.InstanceId, HelloParams.Capabilities.Num());
	HelloSentAt = Clock();
	LastInboundAt = HelloSentAt;
	Transport->Send(SerializeFrame(Hello));
	SetState(EPlayServUplinkState::HelloSent);
}

void FPlayServUplinkClient::HandleConnectionError(const FString& Error)
{
	UE_LOG(LogPlayServ, Warning, TEXT("PlayServ uplink: connection error (%s)"), *Error);
	DropSocket(CloseCodeGoingAway, TEXT("connection_error"));
	ScheduleReconnect(Clock(), 0.0);
}

void FPlayServUplinkClient::HandleClosed(int32 StatusCode, const FString& Reason, bool bWasClean)
{
	const bool bPlatformRefusal = StatusCode == CloseCodePolicyViolation;
	UE_LOG(LogPlayServ, Warning, TEXT("PlayServ uplink: closed (%d %s, clean=%d)"), StatusCode, *Reason, bWasClean ? 1 : 0);
	DropSocket(CloseCodeGoingAway, TEXT("closed"));

	if (bStopped)
	{
		SetState(EPlayServUplinkState::Disconnected);
		return;
	}

	if (bPlatformRefusal)
	{
		const bool bPermanent = IsPermanentRefusal(Reason);
		OnRefused.ExecuteIfBound(Reason, bPermanent);
		if (bPermanent)
		{
			bStopped = true;
			SetState(EPlayServUplinkState::Disconnected);
			UE_LOG(LogPlayServ, Error, TEXT("PlayServ uplink: the platform refused this process (%s); not reconnecting"), *Reason);
			return;
		}
		if (Reason == PlayServRoomsWire::CloseInstanceIdConflict)
		{
			ScheduleReconnect(Clock(), PlayServRoomsWire::InstanceConflictWindowSeconds);
			return;
		}
	}
	ScheduleReconnect(Clock(), 0.0);
}

void FPlayServUplinkClient::HandleMessage(const FString& Message)
{
	LastInboundAt = Clock();

	TSharedPtr<FJsonObject> Frame;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Message);
	if (!FJsonSerializer::Deserialize(Reader, Frame) || !Frame.IsValid())
	{
		UE_LOG(LogPlayServ, Warning, TEXT("PlayServ uplink: malformed frame ignored"));
		return;
	}
	FString Type;
	if (!Frame->TryGetStringField(PlayServRoomsWire::FieldType, Type))
	{
		return;
	}

	if (Type == PlayServRoomsWire::TypePing)
	{
		TSharedPtr<FJsonObject> Pong = MakeShared<FJsonObject>();
		Pong->SetStringField(PlayServRoomsWire::FieldType, PlayServRoomsWire::TypePong);
		if (Transport.IsValid())
		{
			Transport->Send(SerializeFrame(Pong));
		}
		return;
	}

	if (Type == PlayServRoomsWire::TypeHelloAck)
	{
		FString Error;
		FPlayServUplinkAck Parsed;
		if (!FPlayServUplinkAck::Parse(Frame, Parsed, Error))
		{
			UE_LOG(LogPlayServ, Error, TEXT("PlayServ uplink: %s"), *Error);
			DropSocket(CloseCodeProtocolError, TEXT("no hello_ack"));
			ScheduleReconnect(Clock(), 0.0);
			return;
		}
		Ack = Parsed;
		ReconnectAttempt = 0;
		bRenewalPending = false;
		TokenExpiresAt = Ack.ExpiresInSeconds > 0 ? Clock() + static_cast<double>(Ack.ExpiresInSeconds) : 0.0;
		++Generation;
		UE_LOG(LogPlayServ, Display, TEXT("PlayServ uplink: ready (admission=%s, room_config=%s, token expires in %ds)"),
			*Ack.Admission, Ack.bHasRoomConfig ? TEXT("present") : TEXT("null"), Ack.ExpiresInSeconds);
		SetState(EPlayServUplinkState::Ready);
		OnReady.ExecuteIfBound(Ack, Generation);
		return;
	}

	if (Type == PlayServRoomsWire::TypeSessionToken)
	{
		FString Token;
		double ExpiresIn = 0.0;
		if (Frame->TryGetStringField(PlayServRoomsWire::FieldSessionToken, Token) && !Token.IsEmpty())
		{
			Frame->TryGetNumberField(PlayServRoomsWire::FieldExpiresIn, ExpiresIn);
			Ack.SessionToken = Token;
			Ack.ExpiresInSeconds = static_cast<int32>(ExpiresIn);
			TokenExpiresAt = ExpiresIn > 0.0 ? Clock() + ExpiresIn : 0.0;
			bRenewalPending = false;
			OnReady.ExecuteIfBound(Ack, Generation);
		}
		return;
	}

	OnFrame.ExecuteIfBound(Type, Frame);
}

bool FPlayServUplinkClient::SendFrame(const TSharedPtr<FJsonObject>& Frame)
{
	if (State != EPlayServUplinkState::Ready || !Transport.IsValid() || !Frame.IsValid())
	{
		return false;
	}
	const FString Text = SerializeFrame(Frame);
	const FTCHARToUTF8 Converter(*Text);
	if (Converter.Length() > PlayServRoomsWire::MaxFrameBytes)
	{
		UE_LOG(LogPlayServ, Error, TEXT("PlayServ uplink: frame of %d bytes exceeds the 1 MiB cap; not sent"), Converter.Length());
		return false;
	}
	return Transport->Send(Text);
}

void FPlayServUplinkClient::Tick(double Now)
{
	if (!bStarted || bStopped)
	{
		return;
	}

	switch (State)
	{
	case EPlayServUplinkState::Disconnected:
		if (NextConnectAt > 0.0 && Now >= NextConnectAt)
		{
			OpenSocket();
		}
		break;

	case EPlayServUplinkState::Connecting:
		if (Now - LastInboundAt > PlayServRoomsWire::HandshakeDeadlineSeconds * 3.0)
		{
			UE_LOG(LogPlayServ, Warning, TEXT("PlayServ uplink: connect attempt stalled"));
			DropSocket(CloseCodeGoingAway, TEXT("connect_stalled"));
			ScheduleReconnect(Now, 0.0);
		}
		break;

	case EPlayServUplinkState::HelloSent:
		if (Now - HelloSentAt > PlayServRoomsWire::HandshakeDeadlineSeconds)
		{
			UE_LOG(LogPlayServ, Warning, TEXT("PlayServ uplink: no hello ack within %.0fs"), PlayServRoomsWire::HandshakeDeadlineSeconds);
			DropSocket(CloseCodeGoingAway, TEXT("handshake_timeout"));
			ScheduleReconnect(Now, 0.0);
		}
		break;

	case EPlayServUplinkState::Ready:
		if (Now - LastInboundAt > PlayServRoomsWire::DeadSocketSeconds)
		{
			UE_LOG(LogPlayServ, Warning, TEXT("PlayServ uplink: %.0fs without a ping — dead socket"), PlayServRoomsWire::DeadSocketSeconds);
			DropSocket(CloseCodeGoingAway, TEXT("dead_socket"));
			ScheduleReconnect(Now, 0.0);
			break;
		}
		if (TokenExpiresAt > 0.0 && !bRenewalPending && Now >= TokenExpiresAt - RenewalMarginSeconds(Ack.ExpiresInSeconds))
		{
			UE_LOG(LogPlayServ, Display, TEXT("PlayServ uplink: session token nearing expiry — re-hello"));
			bRenewalPending = true;
			DropSocket(CloseCodeGoingAway, TEXT("token_renewal"));
			ReconnectAttempt = 0;
			NextConnectAt = Now;
			SetState(EPlayServUplinkState::Disconnected);
		}
		break;
	}
}
