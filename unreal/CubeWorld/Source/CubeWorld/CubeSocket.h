// A small WebSocket client (RFC 6455: the handshake, masked text frames, ping and close) over OpenSSL's
// connect BIO, plain or TLS. The engine's own client, libwebsockets on Windows, asks the server for
// "//" when the path is the root, and our game servers answer that with 404; this one sends "GET /".
// The pool machines speak wss (TLS with a real certificate), so the TLS side verifies the chain
// against the engine's certificate store and the host name.
#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Dom/JsonObject.h"
#include "Tasks/Task.h"

/** A text frame, parsed: its JSON, and what the socket's decoder read out of it where it was parsed. */
struct FCubeSocketFrame
{
	TSharedPtr<FJsonObject> Json;
	/** A welcome's changed blocks, cell to kind. */
	TOptional<TMap<FIntVector, FName>> World;
};

class FCubeSocket : public TSharedFromThis<FCubeSocket>
{
public:
	DECLARE_MULTICAST_DELEGATE(FOnConnected);
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnError, const FString&);
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnClosed, const FString&);
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnFrame, FCubeSocketFrame&);

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
	/**
	 * Every text frame, parsed, in the order the frames came. A large one is parsed on a worker, so the game thread never
	 * waits for it: a C# server's welcome carries the whole world, and parsing it here took a frame of 115 ms (PSV-3004).
	 */
	FOnFrame OnFrame;
	/** Reads what the game needs out of a parsed frame, beside the parse (on the worker for a large frame): no game state. */
	TFunction<void(FCubeSocketFrame&)> Decode;

private:
	enum class EState : uint8 { Idle, Connecting, Open, Closed };

	bool Tick(float DeltaSeconds);
	void Fail(const FString& Reason);
	void ReadFrames();
	/** A text frame came: parsed here when it is small and nothing waits ahead of it, else on a worker, in turn. */
	void ReceiveText(TArray<uint8> Payload);
	/** Hands the parsed frames on, oldest first, as far as their parse is done (or waits for it). */
	void Deliver(bool bWait);
	friend class FCubeWorldSocketFrameOrderTest;
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

	/** A text frame still being parsed on a worker; frames go out in the order they came, behind it. */
	struct FPending
	{
		TSharedPtr<FCubeSocketFrame, ESPMode::ThreadSafe> Frame;
		UE::Tasks::FTask Parse;
	};
	TArray<FPending> Pending;
};
