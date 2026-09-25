// A small WebSocket client (RFC 6455: the handshake, masked text frames, ping and close) over OpenSSL's
// connect BIO, plain or TLS. The engine's own client, libwebsockets on Windows, asks the server for
// "//" when the path is the root, and our game servers answer that with 404; this one sends "GET /".
// The pool machines speak wss (TLS with a real certificate), so the TLS side verifies the chain
// against the engine's certificate store and the host name.
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"

class FCubeSocket : public TSharedFromThis<FCubeSocket>
{
public:
	DECLARE_MULTICAST_DELEGATE(FOnConnected);
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnError, const FString&);
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnClosed, const FString&);
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnMessage, const FString&);

	FCubeSocket(const FString& Host, int32 Port, bool bSecure, const FString& Path = TEXT("/"));
	~FCubeSocket();

	/** Connects and handshakes on a worker thread; the delegates fire on the game thread. */
	void Connect();
	void Send(const FString& Text);
	void Close();
	bool IsConnected() const { return State == EState::Open; }

	FOnConnected OnConnected;
	FOnError OnError;
	FOnClosed OnClosed;
	FOnMessage OnMessage;

private:
	enum class EState : uint8 { Idle, Connecting, Open, Closed };

	bool Tick(float DeltaSeconds);
	void Fail(const FString& Reason);
	void ReadFrames();
	void SendFrame(uint8 Opcode, const TArray<uint8>& Payload);
	bool DoConnect(FString& OutError);
	void FreeConnection();

	FString Host, Path;
	int32 Port;
	bool bSecure;
	void* Bio = nullptr;      // BIO*
	void* SslContext = nullptr;   // SSL_CTX*
	EState State = EState::Idle;
	TArray<uint8> Incoming;
	FTSTicker::FDelegateHandle Ticker;
	TFuture<void> Worker;
	FThreadSafeBool bWorkerDone = false;
	FString WorkerError;
	FCriticalSection Lock;
};
