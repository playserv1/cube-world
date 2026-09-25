#include "Rooms/PlayServUplinkTransport.h"
#include "Core/PlayServLog.h"
#include "IWebSocket.h"
#include "Modules/ModuleManager.h"
#include "WebSocketsModule.h"

FPlayServWebSocketUplinkTransport::FPlayServWebSocketUplinkTransport(const FString& InUrl, const FString& InBearerCredential)
	: Url(InUrl)
	, BearerCredential(InBearerCredential)
{
}

FPlayServWebSocketUplinkTransport::~FPlayServWebSocketUplinkTransport()
{
	if (Socket.IsValid())
	{
		Socket->OnConnected().Clear();
		Socket->OnConnectionError().Clear();
		Socket->OnMessage().Clear();
		Socket->OnClosed().Clear();
		if (!bCloseRequested && Socket->IsConnected())
		{
			Socket->Close();
		}
		Socket.Reset();
	}
}

void FPlayServWebSocketUplinkTransport::Connect()
{
	if (!Socket.IsValid())
	{
		FWebSocketsModule& Module = FModuleManager::LoadModuleChecked<FWebSocketsModule>(TEXT("WebSockets"));
		TMap<FString, FString> UpgradeHeaders;
		UpgradeHeaders.Add(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *BearerCredential));
		Socket = Module.CreateWebSocket(Url, TArray<FString>(), UpgradeHeaders);
	}

	TWeakPtr<FPlayServWebSocketUplinkTransport> WeakSelf(AsShared());
	Socket->OnConnected().AddLambda([WeakSelf]()
	{
		if (TSharedPtr<FPlayServWebSocketUplinkTransport> Self = WeakSelf.Pin())
		{
			Self->OnConnected.ExecuteIfBound();
		}
	});
	Socket->OnConnectionError().AddLambda([WeakSelf](const FString& Error)
	{
		if (TSharedPtr<FPlayServWebSocketUplinkTransport> Self = WeakSelf.Pin())
		{
			Self->OnConnectionError.ExecuteIfBound(Error);
		}
	});
	Socket->OnMessage().AddLambda([WeakSelf](const FString& Message)
	{
		if (TSharedPtr<FPlayServWebSocketUplinkTransport> Self = WeakSelf.Pin())
		{
			Self->OnMessage.ExecuteIfBound(Message);
		}
	});
	Socket->OnClosed().AddLambda([WeakSelf](int32 StatusCode, const FString& Reason, bool bWasClean)
	{
		if (TSharedPtr<FPlayServWebSocketUplinkTransport> Self = WeakSelf.Pin())
		{
			Self->OnClosed.ExecuteIfBound(StatusCode, Reason, bWasClean);
		}
	});

	UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ uplink: connecting to %s"), *Url);
	Socket->Connect();
}

void FPlayServWebSocketUplinkTransport::Close(int32 Code, const FString& Reason)
{
	if (IsConnected())
	{
		bCloseRequested = true;
		Socket->Close(Code, Reason);
	}
}

bool FPlayServWebSocketUplinkTransport::IsConnected() const
{
	return Socket.IsValid() && !bCloseRequested && Socket->IsConnected();
}

bool FPlayServWebSocketUplinkTransport::Send(const FString& Text)
{
	if (!IsConnected())
	{
		return false;
	}
	Socket->Send(Text);
	return true;
}

FPlayServUplinkTransportFactory FPlayServWebSocketUplinkTransport::MakeFactory()
{
	return [](const FString& Url, const FString& BearerCredential) -> TSharedPtr<FPlayServUplinkTransport>
	{
		return MakeShared<FPlayServWebSocketUplinkTransport>(Url, BearerCredential);
	};
}
