#include "CubeLiveTables.h"
#include "CubeWorld.h"
#include "IWebSocket.h"
#include "WebSocketsModule.h"
#include "Modules/ModuleManager.h"
#include "Core/PlayServSettings.h"
#include "Core/PlayServVersion.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	// The platform's realtime wire, as the PlayServ SDK speaks it.
	const TCHAR* CmdHandshakeRequest = TEXT("HandshakeRequest");
	const TCHAR* CmdHandshakeResponse = TEXT("HandshakeResponse");
	const TCHAR* CmdEventMessage = TEXT("EventMessage");
	const TCHAR* CmdAccessDenied = TEXT("AccessDenied");
	const TCHAR* CmdSubscribe = TEXT("module_dataflow.DataSubscriptionRequest");
	const TCHAR* CmdSubscribeResponse = TEXT("module_dataflow.DataSubscriptionResponse");
	const TCHAR* CmdUpdate = TEXT("module_dataflow.DataSubscriptionUpdate");
	const TCHAR* CmdClose = TEXT("module_dataflow.DataSubscriptionCloseRequest");
	constexpr double ReconnectDelaySeconds = 3.0, RetryDelaySeconds = 5.0;
}

FCubeLiveTables::~FCubeLiveTables()
{
	Shutdown();
}

int32 FCubeLiveTables::Subscribe(const FString& EntityType, const FString& Query, FOnLiveRows OnRows)
{
	const int32 Handle = NextHandle++;
	FSub& Sub = Subs.Add(Handle);
	Sub.EntityType = EntityType;
	Sub.Query = Query;
	Sub.OnRows = MoveTemp(OnRows);
	if (State == EState::Disconnected) Connect();
	else if (State == EState::Ready) Register(Handle, Sub);
	return Handle;
}

void FCubeLiveTables::Unsubscribe(int32 Handle)
{
	FSub* Sub = Subs.Find(Handle);
	if (!Sub) return;
	if (Sub->SubscriptionId != 0 && State == EState::Ready)
	{
		const TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
		Payload->SetNumberField(TEXT("RequestId"), (double)NextRequestId++);
		Payload->SetNumberField(TEXT("SubscriptionId"), (double)Sub->SubscriptionId);
		SendEnvelope(CmdClose, Payload);
	}
	Subs.Remove(Handle);
}

void FCubeLiveTables::Shutdown()
{
	bShuttingDown = true;
	if (TickerHandle.IsValid()) { FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle); TickerHandle.Reset(); }
	if (Socket.IsValid())
	{
		Socket->OnConnected().Clear(); Socket->OnConnectionError().Clear(); Socket->OnMessage().Clear(); Socket->OnClosed().Clear();
		if (Socket->IsConnected()) Socket->Close();
		Socket.Reset();
	}
	Subs.Empty();
	State = EState::Disconnected;
}

void FCubeLiveTables::Connect()
{
	if (bShuttingDown || State != EState::Disconnected) return;
	FString Url = UPlayServSettings::GetBaseURL();
	Url.ReplaceInline(TEXT("https://"), TEXT("wss://"));
	Url.ReplaceInline(TEXT("http://"), TEXT("ws://"));
	Url.RemoveFromEnd(TEXT("/"));
	Url += TEXT("/ws/");

	FWebSocketsModule& Module = FModuleManager::LoadModuleChecked<FWebSocketsModule>(TEXT("WebSockets"));
	Socket = Module.CreateWebSocket(Url);
	TWeakPtr<FCubeLiveTables> Weak(AsShared());
	Socket->OnConnected().AddLambda([Weak]() { if (TSharedPtr<FCubeLiveTables> Self = Weak.Pin()) Self->SendHandshake(); });
	Socket->OnConnectionError().AddLambda([Weak](const FString& Error) { if (TSharedPtr<FCubeLiveTables> Self = Weak.Pin()) Self->OnSocketClosed(-1, Error); });
	Socket->OnMessage().AddLambda([Weak](const FString& Message) { if (TSharedPtr<FCubeLiveTables> Self = Weak.Pin()) Self->OnSocketMessage(Message); });
	Socket->OnClosed().AddLambda([Weak](int32 StatusCode, const FString& Reason, bool) { if (TSharedPtr<FCubeLiveTables> Self = Weak.Pin()) Self->OnSocketClosed(StatusCode, Reason); });
	State = EState::Connecting;
	Socket->Connect();
	if (!TickerHandle.IsValid()) TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateSP(this, &FCubeLiveTables::Tick), 1.f);
}

// The handshake carries the server credential alone: a client key beside it is refused as conflicting.
void FCubeLiveTables::SendHandshake()
{
	if (bShuttingDown) return;
	FString Credential = UPlayServSettings::GetDeploymentToken();
	if (Credential.IsEmpty()) Credential = UPlayServSettings::GetServerKey();
	const TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("GameAccessToken"), TEXT("unused"));
	Payload->SetStringField(TEXT("SdkVersion"), PLAYSERV_SDK_VERSION);
	Payload->SetStringField(TEXT("GameVersion"), TEXT("cubeworld-unreal"));
	Payload->SetStringField(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Credential));
	State = EState::HandshakeSent;
	SendEnvelope(CmdHandshakeRequest, Payload);
}

void FCubeLiveTables::Register(int32 Handle, FSub& Sub)
{
	if (State != EState::Ready || Sub.RequestId != 0 || Sub.SubscriptionId != 0 || FPlatformTime::Seconds() < Sub.RetryAt) return;
	Sub.RequestId = NextRequestId++;
	const TSharedPtr<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetNumberField(TEXT("RequestId"), (double)Sub.RequestId);
	Payload->SetStringField(TEXT("Query"), Sub.Query);
	Payload->SetObjectField(TEXT("Variables"), MakeShared<FJsonObject>());
	SendEnvelope(CmdSubscribe, Payload);
}

void FCubeLiveTables::SendEnvelope(const TCHAR* Command, const TSharedPtr<FJsonObject>& Payload)
{
	if (!Socket.IsValid() || !Socket->IsConnected()) return;
	const TSharedPtr<FJsonObject> Envelope = MakeShared<FJsonObject>();
	Envelope->SetStringField(TEXT("Command"), Command);
	Envelope->SetObjectField(TEXT("Payload"), Payload);
	FString Text;
	const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
	FJsonSerializer::Serialize(Envelope.ToSharedRef(), Writer);
	Socket->Send(Text);
}

void FCubeLiveTables::OnSocketClosed(int32 StatusCode, const FString& Reason)
{
	if (bShuttingDown) return;
	UE_LOG(LogCubeWorld, Warning, TEXT("live tables: socket closed (%d, %s); reconnecting in %.0f s"), StatusCode, *Reason, ReconnectDelaySeconds);
	State = EState::Disconnected;
	Socket.Reset();
	for (auto& Pair : Subs) { Pair.Value.RequestId = 0; Pair.Value.SubscriptionId = 0; }
	ReconnectAt = FPlatformTime::Seconds() + ReconnectDelaySeconds;
}

bool FCubeLiveTables::Tick(float)
{
	if (bShuttingDown) { TickerHandle.Reset(); return false; }
	if (State == EState::Disconnected && Subs.Num() > 0 && FPlatformTime::Seconds() >= ReconnectAt) Connect();
	else if (State == EState::Ready) for (auto& Pair : Subs) Register(Pair.Key, Pair.Value);
	return true;
}

void FCubeLiveTables::OnSocketMessage(const FString& Message)
{
	if (bShuttingDown) return;
	TSharedPtr<FJsonObject> Envelope;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Message), Envelope) || !Envelope.IsValid()) return;
	FString Command;
	const TSharedPtr<FJsonObject>* Payload = nullptr;
	if (!Envelope->TryGetStringField(TEXT("Command"), Command) || !Envelope->TryGetObjectField(TEXT("Payload"), Payload)) return;

	if (Command == CmdEventMessage)
	{
		FString Event;
		if ((*Payload)->TryGetStringField(TEXT("Event"), Event) && Event == TEXT("KeepAlive")) SendEnvelope(CmdEventMessage, *Payload);
	}
	else if (Command == CmdHandshakeResponse)
	{
		bool bOk = false;
		(*Payload)->TryGetBoolField(TEXT("success"), bOk);
		if (!bOk)
		{
			FString Error;
			(*Payload)->TryGetStringField(TEXT("errorMessage"), Error);
			UE_LOG(LogCubeWorld, Warning, TEXT("live tables: handshake refused: %s"), *Error);
			if (Socket.IsValid()) Socket->Close();
			return;
		}
		State = EState::Ready;
		UE_LOG(LogCubeWorld, Log, TEXT("live tables: connected, %d subscription(s) to register"), Subs.Num());
		for (auto& Pair : Subs) Register(Pair.Key, Pair.Value);
	}
	else if (Command == CmdAccessDenied)
	{
		UE_LOG(LogCubeWorld, Warning, TEXT("live tables: a command was refused (access denied)"));
	}
	else if (Command == CmdSubscribeResponse) OnSubscribeResponse(*Payload);
	else if (Command == CmdUpdate) OnUpdate(*Payload);
}

void FCubeLiveTables::OnSubscribeResponse(const TSharedPtr<FJsonObject>& Payload)
{
	double RequestId = 0;
	Payload->TryGetNumberField(TEXT("RequestId"), RequestId);
	const int32 Handle = FindByRequestId((int64)RequestId);
	FSub* Sub = Subs.Find(Handle);
	if (!Sub) return;
	Sub->RequestId = 0;
	const TSharedPtr<FJsonObject>* Result = nullptr;
	if (Payload->TryGetObjectField(TEXT("Result"), Result))
	{
		double SubscriptionId = 0;
		(*Result)->TryGetNumberField(TEXT("SubscriptionId"), SubscriptionId);
		Sub->SubscriptionId = (int64)SubscriptionId;
		return;
	}
	const TSharedPtr<FJsonObject>* Error = nullptr;
	if (Payload->TryGetObjectField(TEXT("Error"), Error))
	{
		double Code = 0;
		FString Text;
		(*Error)->TryGetNumberField(TEXT("ErrorCode"), Code);
		(*Error)->TryGetStringField(TEXT("Message"), Text);
		UE_LOG(LogCubeWorld, Warning, TEXT("live tables: %s refused (%d): %s; trying again in %.0f s"), *Sub->EntityType, (int32)Code, *Text, RetryDelaySeconds);
		Sub->RetryAt = FPlatformTime::Seconds() + RetryDelaySeconds;
	}
}

void FCubeLiveTables::OnUpdate(const TSharedPtr<FJsonObject>& Payload)
{
	double SubscriptionId = 0;
	Payload->TryGetNumberField(TEXT("DataSubscriptionId"), SubscriptionId);
	const int32 Handle = FindBySubscriptionId((int64)SubscriptionId);
	FSub* Sub = Subs.Find(Handle);
	if (!Sub) return;
	FString UpdateType;
	Payload->TryGetStringField(TEXT("UpdateType"), UpdateType);
	if (UpdateType == TEXT("Terminated"))
	{
		double Code = 0;
		FString Text;
		Payload->TryGetNumberField(TEXT("ErrorCode"), Code);
		Payload->TryGetStringField(TEXT("Message"), Text);
		UE_LOG(LogCubeWorld, Warning, TEXT("live tables: %s ended by the platform (%d): %s; subscribing again in %.0f s"), *Sub->EntityType, (int32)Code, *Text, RetryDelaySeconds);
		Sub->SubscriptionId = 0;
		Sub->RetryAt = FPlatformTime::Seconds() + RetryDelaySeconds;
		return;
	}
	if (UpdateType != TEXT("Overwrite")) return;
	const TSharedPtr<FJsonObject>* Data = nullptr;
	if (!Payload->TryGetObjectField(TEXT("Data"), Data)) return;
	const TArray<TSharedPtr<FJsonValue>>* RowsJson = nullptr;
	if (!(*Data)->TryGetArrayField(Sub->EntityType, RowsJson)) return;
	TArray<TSharedPtr<FJsonObject>> Rows;
	for (const TSharedPtr<FJsonValue>& V : *RowsJson)
	{
		const TSharedPtr<FJsonObject>* Row = nullptr;
		if (V.IsValid() && V->TryGetObject(Row)) Rows.Add(*Row);
	}
	// The delegate may subscribe again (a moved window); it runs on a copy so the map may change under it.
	const FOnLiveRows OnRows = Sub->OnRows;
	OnRows.ExecuteIfBound(Rows);
}

int32 FCubeLiveTables::FindBySubscriptionId(int64 SubscriptionId) const
{
	if (SubscriptionId == 0) return 0;
	for (const auto& Pair : Subs) if (Pair.Value.SubscriptionId == SubscriptionId) return Pair.Key;
	return 0;
}

int32 FCubeLiveTables::FindByRequestId(int64 RequestId) const
{
	if (RequestId == 0) return 0;
	for (const auto& Pair : Subs) if (Pair.Value.RequestId == RequestId) return Pair.Key;
	return 0;
}
