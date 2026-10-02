// A small WebSocket server (RFC 6455: the upgrade handshake, masked text frames from the clients, ping and
// close) over the engine's TCP sockets, polled from the game thread. The dedicated server listens on it beside
// its Unreal port, so a browser client speaks to it in the JSON frames the C# server speaks. Plain ws; TLS in
// front of it is the machine's business.
#pragma once

#include "CoreMinimal.h"

class FSocket;

class FCubeWebSocketServer
{
public:
	/** A text frame from a client. */
	DECLARE_DELEGATE_TwoParams(FOnText, int32 /*Client*/, const FString&);
	/** A client's socket closed, by them or by us. */
	DECLARE_DELEGATE_OneParam(FOnClosed, int32 /*Client*/);

	~FCubeWebSocketServer();

	bool Listen(int32 Port);
	/** Accepts, reads and dispatches; call every tick. */
	void Tick();
	void Send(int32 Client, const FString& Text);
	/** Sends a close frame with a code and a reason, then drops the socket. */
	void Close(int32 Client, uint16 Code, const FString& Reason);

	/**
	 * The close frame that answers a peer's: its own status code, or 1000 when it gave none (a browser's bare close()),
	 * as the C# servers answer. An empty answer reached the browser as 1005, "no status".
	 */
	static TArray<uint8> CloseAnswer(const TArray<uint8>& PeerClosePayload);
	void Shutdown();
	int32 ListeningPort() const { return Port; }

	FOnText OnText;
	FOnClosed OnClosed;

private:
	struct FClient
	{
		FSocket* Socket = nullptr;
		TArray<uint8> Incoming;
		bool bUpgraded = false;
		double OpenedAt = 0;
	};

	void ReadClient(int32 Id, FClient& Client);
	bool Handshake(int32 Id, FClient& Client);
	void ReadFrames(int32 Id, FClient& Client);
	bool SendFrame(FClient& Client, uint8 Opcode, const TArray<uint8>& Payload);
	void Drop(int32 Id, bool bTell);

	FSocket* Listener = nullptr;
	TMap<int32, FClient> Clients;
	int32 NextId = 1;
	int32 Port = 0;
};
