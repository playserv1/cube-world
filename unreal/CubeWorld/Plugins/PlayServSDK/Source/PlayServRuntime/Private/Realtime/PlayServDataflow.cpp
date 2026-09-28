#include "Realtime/PlayServDataflow.h"
#include "Auth/PlayServAuth.h"
#include "Core/PlayServLog.h"
#include "Core/PlayServSettings.h"
#include "Core/PlayServSubsystem.h"
#include "Core/PlayServVersion.h"
#include "IWebSocket.h"
#include "WebSocketsModule.h"
#include "Modules/ModuleManager.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace PlayServDataflowWire
{
	static const TCHAR* CmdHandshakeRequest = TEXT("HandshakeRequest");
	static const TCHAR* CmdHandshakeResponse = TEXT("HandshakeResponse");
	static const TCHAR* CmdRefreshAuthRequest = TEXT("RefreshAuthRequest");
	static const TCHAR* CmdRefreshAuthResponse = TEXT("RefreshAuthResponse");
	static const TCHAR* CmdEventMessage = TEXT("EventMessage");
	static const TCHAR* CmdAccessDenied = TEXT("AccessDenied");
	static const TCHAR* CmdSubscribe = TEXT("module_dataflow.DataSubscriptionRequest");
	static const TCHAR* CmdSubscribeResponse = TEXT("module_dataflow.DataSubscriptionResponse");
	static const TCHAR* CmdUpdate = TEXT("module_dataflow.DataSubscriptionUpdate");
	static const TCHAR* CmdClose = TEXT("module_dataflow.DataSubscriptionCloseRequest");
	static const TCHAR* CmdCloseResponse = TEXT("module_dataflow.DataSubscriptionCloseResponse");

	static constexpr int32 CodeRowDeleted = 49001;
	static constexpr int32 CodeConnectionLost = -1;

	static constexpr float MaintenanceIntervalSeconds = 30.0f;
}

#if !UE_BUILD_SHIPPING
int32 FPlayServDataflow::TestTerminatedFrameCount = 0;
int32 FPlayServDataflow::TestLastTerminatedCode = 0;
int32 FPlayServDataflow::TestRegisteredRowCount = 0;
#endif

FPlayServDataflow::~FPlayServDataflow()
{
	Shutdown();
}

FString FPlayServDataflow::MakeKey(const FString& EntityType, const FString& RecordId)
{
	return FString::Printf(TEXT("%s|%s"), *EntityType, *RecordId);
}

void FPlayServDataflow::SubscribeRecord(const FString& EntityType, const FString& RecordId)
{
	FRowSubscription& Row = RowsByKey.FindOrAdd(MakeKey(EntityType, RecordId));
	const bool bNewRow = Row.RefCount == 0;
	Row.EntityType = EntityType;
	Row.RecordId = RecordId;
	++Row.RefCount;

	EnsureConnected();
	if (bNewRow && State == EConnectionState::Ready)
	{
		RegisterRow(Row);
	}
}

void FPlayServDataflow::UnsubscribeRecord(const FString& EntityType, const FString& RecordId)
{
	const FString Key = MakeKey(EntityType, RecordId);
	FRowSubscription* Row = RowsByKey.Find(Key);
	if (Row == nullptr)
	{
		return;
	}
	if (--Row->RefCount > 0)
	{
		return;
	}

	CloseRowOnWire(*Row);
	RowsByKey.Remove(Key);
}

void FPlayServDataflow::CloseRowOnWire(const FRowSubscription& Row)
{
	if (Row.WsSubscriptionId == 0 || !Socket.IsValid() || State != EConnectionState::Ready)
	{
		return;
	}
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetNumberField(TEXT("RequestId"), static_cast<double>(NextRequestId++));
	Payload->SetNumberField(TEXT("SubscriptionId"), static_cast<double>(Row.WsSubscriptionId));
	SendEnvelope(PlayServDataflowWire::CmdClose, Payload);
}

void FPlayServDataflow::Shutdown()
{
	bShuttingDown = true;
	if (MaintenanceTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(MaintenanceTickerHandle);
		MaintenanceTickerHandle.Reset();
	}
	if (Socket.IsValid())
	{
		if (Socket->IsConnected())
		{
			Socket->Close();
		}
		Socket.Reset();
	}
	RowsByKey.Empty();
	State = EConnectionState::Disconnected;
	LastSentAccessToken.Empty();
}

#if !UE_BUILD_SHIPPING
void FPlayServDataflow::TestSimulatePlatformClose(int32 StatusCode)
{
	if (Socket.IsValid())
	{
		Socket->OnConnected().Clear();
		Socket->OnConnectionError().Clear();
		Socket->OnMessage().Clear();
		Socket->OnClosed().Clear();
		Socket->Close();
	}
	OnSocketClosed(StatusCode, TEXT("Service Restart"), true);
}
#endif

void FPlayServDataflow::EnsureConnected()
{
	if (bShuttingDown || State != EConnectionState::Disconnected)
	{
		return;
	}

	FString Url = UPlayServSettings::GetBaseURL();
	Url.ReplaceInline(TEXT("https://"), TEXT("wss://"));
	Url.ReplaceInline(TEXT("http://"), TEXT("ws://"));
	Url.RemoveFromEnd(TEXT("/"));
	Url += TEXT("/ws/");

	FWebSocketsModule& Module = FModuleManager::LoadModuleChecked<FWebSocketsModule>(TEXT("WebSockets"));
	Socket = Module.CreateWebSocket(Url);

	TWeakPtr<FPlayServDataflow> WeakSelf(AsShared());
	Socket->OnConnected().AddLambda([WeakSelf]()
	{
		if (TSharedPtr<FPlayServDataflow> Self = WeakSelf.Pin())
		{
			Self->OnSocketConnected();
		}
	});
	Socket->OnConnectionError().AddLambda([WeakSelf](const FString& Error)
	{
		if (TSharedPtr<FPlayServDataflow> Self = WeakSelf.Pin())
		{
			Self->OnSocketConnectionError(Error);
		}
	});
	Socket->OnMessage().AddLambda([WeakSelf](const FString& Message)
	{
		if (TSharedPtr<FPlayServDataflow> Self = WeakSelf.Pin())
		{
			Self->OnSocketMessage(Message);
		}
	});
	Socket->OnClosed().AddLambda([WeakSelf](int32 StatusCode, const FString& Reason, bool bWasClean)
	{
		if (TSharedPtr<FPlayServDataflow> Self = WeakSelf.Pin())
		{
			Self->OnSocketClosed(StatusCode, Reason, bWasClean);
		}
	});

	State = EConnectionState::Connecting;
	UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ dataflow: connecting"));
	Socket->Connect();

	if (!MaintenanceTickerHandle.IsValid())
	{
		MaintenanceTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateSP(this, &FPlayServDataflow::TickMaintenance),
			PlayServDataflowWire::MaintenanceIntervalSeconds);
	}
}

void FPlayServDataflow::SendHandshake()
{
	UPlayServSubsystem* Subsystem = UPlayServSubsystem::Get();
	UPlayServAuth* Auth = Subsystem ? Subsystem->GetAuth() : nullptr;
	const FString AccessToken = Auth ? Auth->GetAccessToken() : FString();

	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("GameAccessToken"), TEXT("unused"));
	Payload->SetStringField(TEXT("SdkVersion"), PLAYSERV_SDK_VERSION);
	Payload->SetStringField(TEXT("GameVersion"), TEXT("unused"));
	Payload->SetStringField(TEXT("UserId"), TEXT("unused"));
	Payload->SetStringField(TEXT("GameId"), TEXT("unused"));
	Payload->SetStringField(TEXT("ClientToken"), UPlayServSettings::GetClientKey());
	Payload->SetStringField(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *AccessToken));

	LastSentAccessToken = AccessToken;
	State = EConnectionState::HandshakeSent;
	SendEnvelope(PlayServDataflowWire::CmdHandshakeRequest, Payload);
}

void FPlayServDataflow::OnSocketConnected()
{
	if (bShuttingDown)
	{
		return;
	}
	SendHandshake();
}

void FPlayServDataflow::OnSocketConnectionError(const FString& Error)
{
	UE_LOG(LogPlayServ, Warning, TEXT("PlayServ dataflow: connection error: %s"), *Error);
	State = EConnectionState::Disconnected;
	Socket.Reset();
	if (!bShuttingDown && !bReconnectAttempted && RowsByKey.Num() > 0)
	{
		bReconnectAttempted = true;
		EnsureConnected();
		return;
	}
	FailAllSubscriptions();
}

void FPlayServDataflow::OnSocketClosed(int32 StatusCode, const FString& Reason, bool bWasClean)
{
	UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ dataflow: socket closed (%d, %s, clean=%d)"), StatusCode, *Reason, bWasClean ? 1 : 0);
	State = EConnectionState::Disconnected;
	Socket.Reset();
	for (TPair<FString, FRowSubscription>& Pair : RowsByKey)
	{
		Pair.Value.WsSubscriptionId = 0;
		Pair.Value.RegisterRequestId = 0;
	}
	if (bShuttingDown || RowsByKey.Num() == 0)
	{
		return;
	}
	if (!bReconnectAttempted)
	{
		bReconnectAttempted = true;
		EnsureConnected();
		return;
	}
	FailAllSubscriptions();
}

void FPlayServDataflow::FailAllSubscriptions()
{
	if (RowsByKey.Num() == 0)
	{
		return;
	}
	RowsByKey.Empty();
	OnConnectionLost.ExecuteIfBound();
}

void FPlayServDataflow::RegisterRow(FRowSubscription& Row)
{
	if (Row.RegisterRequestId != 0 || Row.WsSubscriptionId != 0 || State != EConnectionState::Ready)
	{
		return;
	}
	Row.RegisterRequestId = NextRequestId++;
	TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetNumberField(TEXT("RequestId"), static_cast<double>(Row.RegisterRequestId));
	Payload->SetStringField(TEXT("Query"),
		FString::Printf(TEXT("%s(id: \"%s\") { * }"), *Row.EntityType, *Row.RecordId));
	Payload->SetObjectField(TEXT("Variables"), MakeShared<FJsonObject>());
	SendEnvelope(PlayServDataflowWire::CmdSubscribe, Payload);
}

void FPlayServDataflow::SendEnvelope(const FString& Command, const TSharedPtr<FJsonObject>& Payload)
{
	if (!Socket.IsValid() || !Socket->IsConnected())
	{
		return;
	}
	TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
	Envelope->SetStringField(TEXT("Command"), Command);
	Envelope->SetObjectField(TEXT("Payload"), Payload);

	FString Serialized;
	TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
		TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Serialized);
	FJsonSerializer::Serialize(Envelope.ToSharedRef(), Writer);
	Socket->Send(Serialized);
}

void FPlayServDataflow::OnSocketMessage(const FString& Message)
{
	if (bShuttingDown)
	{
		return;
	}

	TSharedPtr<FJsonObject> Envelope;
	TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Message);
	if (!FJsonSerializer::Deserialize(Reader, Envelope) || !Envelope.IsValid())
	{
		return;
	}
	FString Command;
	const TSharedPtr<FJsonObject>* Payload = nullptr;
	if (!Envelope->TryGetStringField(TEXT("Command"), Command) || !Envelope->TryGetObjectField(TEXT("Payload"), Payload))
	{
		return;
	}

	if (Command == PlayServDataflowWire::CmdEventMessage)
	{
		FString EventName;
		if ((*Payload)->TryGetStringField(TEXT("Event"), EventName) && EventName == TEXT("KeepAlive"))
		{
			SendEnvelope(Command, *Payload);
		}
		return;
	}

	if (Command == PlayServDataflowWire::CmdHandshakeResponse)
	{
		bool bSuccess = false;
		(*Payload)->TryGetBoolField(TEXT("success"), bSuccess);
		if (!bSuccess)
		{
			FString ErrorMessage;
			(*Payload)->TryGetStringField(TEXT("errorMessage"), ErrorMessage);
			UE_LOG(LogPlayServ, Warning, TEXT("PlayServ dataflow: handshake rejected: %s"), *ErrorMessage);
			return;
		}
		State = EConnectionState::Ready;
		bReconnectAttempted = false;
		UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ dataflow: handshake OK"));
		for (TPair<FString, FRowSubscription>& Pair : RowsByKey)
		{
			RegisterRow(Pair.Value);
		}
		return;
	}

	if (Command == PlayServDataflowWire::CmdRefreshAuthResponse)
	{
		bool bSuccess = false;
		(*Payload)->TryGetBoolField(TEXT("success"), bSuccess);
		if (!bSuccess)
		{
			FString RefreshError;
			(*Payload)->TryGetStringField(TEXT("message"), RefreshError);
			UE_LOG(LogPlayServ, Warning, TEXT("PlayServ dataflow: in-band auth refresh failed: %s"), *RefreshError);
		}
		return;
	}

	if (Command == PlayServDataflowWire::CmdAccessDenied)
	{
		UE_LOG(LogPlayServ, Warning, TEXT("PlayServ dataflow: command refused (player session expired) — refreshing in-band on the next tick"));
		return;
	}

	if (Command == PlayServDataflowWire::CmdSubscribeResponse)
	{
		HandleSubscriptionResponse(*Payload);
		return;
	}

	if (Command == PlayServDataflowWire::CmdUpdate)
	{
		HandleSubscriptionUpdate(*Payload);
		return;
	}
}

void FPlayServDataflow::HandleSubscriptionResponse(const TSharedPtr<FJsonObject>& Payload)
{
	double RequestIdRaw = 0.0;
	Payload->TryGetNumberField(TEXT("RequestId"), RequestIdRaw);
	const int64 RequestId = static_cast<int64>(RequestIdRaw);

	FRowSubscription* Row = FindRowByRequestId(RequestId);
	if (Row == nullptr)
	{
		return;
	}
	Row->RegisterRequestId = 0;

	const TSharedPtr<FJsonObject>* Result = nullptr;
	if (Payload->TryGetObjectField(TEXT("Result"), Result))
	{
		double SubscriptionIdRaw = 0.0;
		(*Result)->TryGetNumberField(TEXT("SubscriptionId"), SubscriptionIdRaw);
		Row->WsSubscriptionId = static_cast<int64>(SubscriptionIdRaw);
#if !UE_BUILD_SHIPPING
		++TestRegisteredRowCount;
#endif
		UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ dataflow: %s %s registered (subscription %lld)"),
			*Row->EntityType, *Row->RecordId, Row->WsSubscriptionId);
		return;
	}

	const TSharedPtr<FJsonObject>* Error = nullptr;
	if (Payload->TryGetObjectField(TEXT("Error"), Error))
	{
		double CodeRaw = 0.0;
		FString ErrorMessage;
		(*Error)->TryGetNumberField(TEXT("ErrorCode"), CodeRaw);
		(*Error)->TryGetStringField(TEXT("Message"), ErrorMessage);
		UE_LOG(LogPlayServ, Warning, TEXT("PlayServ dataflow: subscribe to %s %s refused (%d): %s"),
			*Row->EntityType, *Row->RecordId, static_cast<int32>(CodeRaw), *ErrorMessage);

		const FString EntityType = Row->EntityType;
		const FString RecordId = Row->RecordId;
		RowsByKey.Remove(MakeKey(EntityType, RecordId));
		OnTerminated.ExecuteIfBound(EntityType, RecordId, static_cast<int32>(CodeRaw));
	}
}

void FPlayServDataflow::HandleSubscriptionUpdate(const TSharedPtr<FJsonObject>& Payload)
{
	double SubscriptionIdRaw = 0.0;
	Payload->TryGetNumberField(TEXT("DataSubscriptionId"), SubscriptionIdRaw);
	const int64 SubscriptionId = static_cast<int64>(SubscriptionIdRaw);

	FString UpdateType;
	Payload->TryGetStringField(TEXT("UpdateType"), UpdateType);

	FRowSubscription* Row = FindRowBySubscriptionId(SubscriptionId);
	if (Row == nullptr)
	{
		FString EntityType;
		FString IdValue;
		if (Payload->TryGetStringField(TEXT("EntityType"), EntityType) &&
			Payload->TryGetStringField(TEXT("IdValue"), IdValue))
		{
			Row = RowsByKey.Find(MakeKey(EntityType, IdValue));
			if (Row != nullptr && UpdateType == TEXT("Overwrite"))
			{
				Row->WsSubscriptionId = SubscriptionId;
				UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ dataflow: %s %s restored (subscription %lld)"),
					*EntityType, *IdValue, SubscriptionId);
			}
		}
	}
	if (Row == nullptr)
	{
		return;
	}

	if (UpdateType == TEXT("Terminated"))
	{
		double CodeRaw = 0.0;
		Payload->TryGetNumberField(TEXT("ErrorCode"), CodeRaw);
		const int32 Code = static_cast<int32>(CodeRaw);
#if !UE_BUILD_SHIPPING
		++TestTerminatedFrameCount;
		TestLastTerminatedCode = Code;
#endif
		const FString EntityType = Row->EntityType;
		const FString RecordId = Row->RecordId;
		if (Code == PlayServDataflowWire::CodeRowDeleted)
		{
			UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ dataflow: %s %s deleted server-side (49001)"),
				*EntityType, *RecordId);
		}
		else
		{
			UE_LOG(LogPlayServ, Warning, TEXT("PlayServ dataflow: %s %s subscription terminated by the platform (code %d)"),
				*EntityType, *RecordId, Code);
		}
		RowsByKey.Remove(MakeKey(EntityType, RecordId));
		OnTerminated.ExecuteIfBound(EntityType, RecordId, Code);
		return;
	}

	if (UpdateType != TEXT("Overwrite"))
	{
		return;
	}

	const TSharedPtr<FJsonObject>* Data = nullptr;
	if (!Payload->TryGetObjectField(TEXT("Data"), Data))
	{
		return;
	}

	if ((*Data)->Values.Num() == 1)
	{
		FString OnlyValue;
		const TSharedPtr<FJsonValue> Single = (*Data)->Values.CreateConstIterator()->Value;
		if (Single.IsValid() && Single->TryGetString(OnlyValue) && OnlyValue == Row->RecordId)
		{
			UE_LOG(LogPlayServ, Verbose,
				TEXT("PlayServ dataflow: %s %s — platform reports no such row; keeping local state"),
				*Row->EntityType, *Row->RecordId);
			return;
		}
	}

	OnUpdate.ExecuteIfBound(Row->EntityType, Row->RecordId, *Data);
}

bool FPlayServDataflow::TickMaintenance(float)
{
	if (bShuttingDown)
	{
		MaintenanceTickerHandle.Reset();
		return false;
	}

	if (State == EConnectionState::Ready)
	{
		UPlayServSubsystem* Subsystem = UPlayServSubsystem::Get();
		UPlayServAuth* Auth = Subsystem ? Subsystem->GetAuth() : nullptr;
		const FString CurrentToken = Auth ? Auth->GetAccessToken() : FString();
		if (!CurrentToken.IsEmpty() && CurrentToken != LastSentAccessToken)
		{
			TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
			Payload->SetStringField(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *CurrentToken));
			LastSentAccessToken = CurrentToken;
			SendEnvelope(PlayServDataflowWire::CmdRefreshAuthRequest, Payload);
		}

		for (TPair<FString, FRowSubscription>& Pair : RowsByKey)
		{
			RegisterRow(Pair.Value);
		}
	}

	return true;
}

FPlayServDataflow::FRowSubscription* FPlayServDataflow::FindRowBySubscriptionId(int64 SubscriptionId)
{
	if (SubscriptionId == 0)
	{
		return nullptr;
	}
	for (TPair<FString, FRowSubscription>& Pair : RowsByKey)
	{
		if (Pair.Value.WsSubscriptionId == SubscriptionId)
		{
			return &Pair.Value;
		}
	}
	return nullptr;
}

FPlayServDataflow::FRowSubscription* FPlayServDataflow::FindRowByRequestId(int64 RequestId)
{
	if (RequestId == 0)
	{
		return nullptr;
	}
	for (TPair<FString, FRowSubscription>& Pair : RowsByKey)
	{
		if (Pair.Value.RegisterRequestId == RequestId)
		{
			return &Pair.Value;
		}
	}
	return nullptr;
}
