#pragma once

#include "CoreMinimal.h"

class IWebSocket;

class FPlayServUplinkTransport
{
public:
	DECLARE_DELEGATE(FOnTransportConnected);
	DECLARE_DELEGATE_OneParam(FOnTransportConnectionError, const FString&);
	DECLARE_DELEGATE_ThreeParams(FOnTransportClosed, int32, const FString&, bool);
	DECLARE_DELEGATE_OneParam(FOnTransportMessage, const FString&);

	virtual ~FPlayServUplinkTransport() = default;

	virtual void Connect() = 0;
	virtual void Close(int32 Code, const FString& Reason) = 0;
	virtual bool IsConnected() const = 0;
	virtual bool Send(const FString& Text) = 0;

	FOnTransportConnected OnConnected;
	FOnTransportConnectionError OnConnectionError;
	FOnTransportClosed OnClosed;
	FOnTransportMessage OnMessage;
};

using FPlayServUplinkTransportFactory = TFunction<TSharedPtr<FPlayServUplinkTransport>(const FString&, const FString&)>;

class PLAYSERVRUNTIME_API FPlayServWebSocketUplinkTransport : public FPlayServUplinkTransport, public TSharedFromThis<FPlayServWebSocketUplinkTransport>
{
public:
	FPlayServWebSocketUplinkTransport(const FString& InUrl, const FString& InBearerCredential);
	virtual ~FPlayServWebSocketUplinkTransport() override;

	virtual void Connect() override;
	virtual void Close(int32 Code, const FString& Reason) override;
	virtual bool IsConnected() const override;
	virtual bool Send(const FString& Text) override;

	static FPlayServUplinkTransportFactory MakeFactory();

private:
#if !UE_BUILD_SHIPPING
	friend class FPlayServUplinkTransportTestAccess;
#endif

	FString Url;
	FString BearerCredential;
	TSharedPtr<IWebSocket> Socket;
	bool bCloseRequested = false;
};
