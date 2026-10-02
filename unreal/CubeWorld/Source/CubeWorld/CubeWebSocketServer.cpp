#include "CubeWebSocketServer.h"
#include "CubeWorld.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
#include "Misc/Base64.h"
#include "Misc/SecureHash.h"

namespace
{
	constexpr int32 MaxFrameBytes = 1024 * 1024;
	constexpr double HandshakeSeconds = 10.0;
}

FCubeWebSocketServer::~FCubeWebSocketServer()
{
	Shutdown();
}

bool FCubeWebSocketServer::Listen(int32 InPort)
{
	ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!Sockets) return false;
	Listener = Sockets->CreateSocket(NAME_Stream, TEXT("CubeWebSocketServer"), false);
	if (!Listener) return false;
	TSharedRef<FInternetAddr> Addr = Sockets->CreateInternetAddr();
	Addr->SetAnyAddress();
	Addr->SetPort(InPort);
	Listener->SetReuseAddr(true);
	Listener->SetNonBlocking(true);
	if (!Listener->Bind(*Addr) || !Listener->Listen(16))
	{
		UE_LOG(LogCubeWorld, Warning, TEXT("websocket: cannot listen on %d"), InPort);
		Sockets->DestroySocket(Listener);
		Listener = nullptr;
		return false;
	}
	Port = InPort;
	UE_LOG(LogCubeWorld, Log, TEXT("websocket: listening on %d"), InPort);
	return true;
}

void FCubeWebSocketServer::Shutdown()
{
	ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	for (auto& Pair : Clients)
	{
		if (Pair.Value.Socket) { Pair.Value.Socket->Close(); if (Sockets) Sockets->DestroySocket(Pair.Value.Socket); }
	}
	Clients.Empty();
	if (Listener) { Listener->Close(); if (Sockets) Sockets->DestroySocket(Listener); Listener = nullptr; }
}

void FCubeWebSocketServer::Tick()
{
	if (!Listener) return;
	bool bPending = false;
	while (Listener->HasPendingConnection(bPending) && bPending)
	{
		FSocket* Accepted = Listener->Accept(TEXT("CubeWebSocketClient"));
		if (!Accepted) break;
		Accepted->SetNonBlocking(true);
		Accepted->SetNoDelay(true);
		FClient& Client = Clients.Add(NextId++);
		Client.Socket = Accepted;
		Client.OpenedAt = FPlatformTime::Seconds();
	}
	TArray<int32> Ids;
	Clients.GetKeys(Ids);
	for (const int32 Id : Ids)
	{
		FClient* Client = Clients.Find(Id);
		if (Client) ReadClient(Id, *Client);
	}
}

void FCubeWebSocketServer::ReadClient(int32 Id, FClient& Client)
{
	for (int32 Rounds = 0; Rounds < 64; Rounds++)
	{
		uint32 Waiting = 0;
		if (!Client.Socket->HasPendingData(Waiting) || Waiting == 0) break;
		uint8 Buffer[16384];
		int32 Read = 0;
		if (!Client.Socket->Recv(Buffer, sizeof(Buffer), Read) || Read <= 0) { Drop(Id, true); return; }
		Client.Incoming.Append(Buffer, Read);
		if (Client.Incoming.Num() > MaxFrameBytes + 16) { Drop(Id, true); return; }
	}
	if (Client.Socket->GetConnectionState() == SCS_ConnectionError) { Drop(Id, true); return; }
	if (!Client.bUpgraded)
	{
		if (FPlatformTime::Seconds() - Client.OpenedAt > HandshakeSeconds && Client.Incoming.Num() == 0) { Drop(Id, false); return; }
		if (!Handshake(Id, Client)) return;
	}
	ReadFrames(Id, Client);
}

// The upgrade request: answer it once its headers are all here.
bool FCubeWebSocketServer::Handshake(int32 Id, FClient& Client)
{
	const FUTF8ToTCHAR Converted((const ANSICHAR*)Client.Incoming.GetData(), Client.Incoming.Num());
	const FString Text(Converted.Length(), Converted.Get());
	const int32 End = Text.Find(TEXT("\r\n\r\n"));
	if (End < 0) return false;
	FString Key;
	for (const FString& Line : [&]() { TArray<FString> Lines; Text.Left(End).ParseIntoArray(Lines, TEXT("\r\n")); return Lines; }())
	{
		if (Line.StartsWith(TEXT("Sec-WebSocket-Key:"), ESearchCase::IgnoreCase)) Key = Line.Mid(18).TrimStartAndEnd();
	}
	if (Key.IsEmpty())
	{
		const FString Refusal = TEXT("HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n");
		const FTCHARToUTF8 Utf8(*Refusal);
		int32 Sent = 0;
		Client.Socket->Send((const uint8*)Utf8.Get(), Utf8.Length(), Sent);
		Drop(Id, false);
		return false;
	}
	const FTCHARToUTF8 KeyUtf8(*(Key + TEXT("258EAFA5-E914-47DA-95CA-C5AB0DC85B11")));
	uint8 Digest[20];
	FSHA1::HashBuffer(KeyUtf8.Get(), KeyUtf8.Length(), Digest);
	const FString Answer = FString::Printf(TEXT("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n"), *FBase64::Encode(Digest, 20));
	const FTCHARToUTF8 AnswerUtf8(*Answer);
	int32 Sent = 0;
	if (!Client.Socket->Send((const uint8*)AnswerUtf8.Get(), AnswerUtf8.Length(), Sent)) { Drop(Id, false); return false; }
	// Whatever followed the headers is already a frame.
	const FTCHARToUTF8 HeadUtf8(*Text.Left(End + 4));
	Client.Incoming.RemoveAt(0, HeadUtf8.Length(), EAllowShrinking::No);
	Client.bUpgraded = true;
	return true;
}

void FCubeWebSocketServer::ReadFrames(int32 Id, FClient& Client)
{
	while (Client.Incoming.Num() >= 2)
	{
		const uint8 B0 = Client.Incoming[0], B1 = Client.Incoming[1];
		const uint8 Opcode = B0 & 0x0F;
		const bool bMasked = (B1 & 0x80) != 0;
		uint64 Length = B1 & 0x7F;
		int32 Offset = 2;
		if (Length == 126) { if (Client.Incoming.Num() < 4) return; Length = ((uint64)Client.Incoming[2] << 8) | Client.Incoming[3]; Offset = 4; }
		else if (Length == 127) { if (Client.Incoming.Num() < 10) return; Length = 0; for (int32 I = 0; I < 8; I++) Length = (Length << 8) | Client.Incoming[2 + I]; Offset = 10; }
		if (Length > (uint64)MaxFrameBytes) { Close(Id, 1009, TEXT("message too large")); return; }
		if (bMasked) Offset += 4;
		if ((uint64)Client.Incoming.Num() < Offset + Length) return;

		TArray<uint8> Payload;
		Payload.Append(Client.Incoming.GetData() + Offset, (int32)Length);
		if (bMasked) for (int32 I = 0; I < Payload.Num(); I++) Payload[I] ^= Client.Incoming[Offset - 4 + (I % 4)];
		Client.Incoming.RemoveAt(0, Offset + (int32)Length, EAllowShrinking::No);

		if (Opcode == 0x1)
		{
			const FUTF8ToTCHAR Converted((const ANSICHAR*)Payload.GetData(), Payload.Num());
			OnText.ExecuteIfBound(Id, FString(Converted.Length(), Converted.Get()));
			if (!Clients.Contains(Id)) return;
		}
		else if (Opcode == 0x9) SendFrame(Client, 0xA, Payload);
		else if (Opcode == 0x8)
		{
			// The peer is closing: answer with a close frame, then drop, so they see a clean close rather than a lost connection.
			SendFrame(Client, 0x8, CloseAnswer(Payload));
			Drop(Id, true);
			return;
		}
	}
}

bool FCubeWebSocketServer::SendFrame(FClient& Client, uint8 Opcode, const TArray<uint8>& Payload)
{
	TArray<uint8> Frame;
	Frame.Add(0x80 | Opcode);
	const int32 N = Payload.Num();
	if (N < 126) Frame.Add((uint8)N);
	else if (N < 65536) { Frame.Add(126); Frame.Add((uint8)(N >> 8)); Frame.Add((uint8)N); }
	else { Frame.Add(127); for (int32 I = 7; I >= 0; I--) Frame.Add((uint8)(((uint64)N >> (I * 8)) & 0xFF)); }
	Frame.Append(Payload);
	int32 Total = 0;
	const double Deadline = FPlatformTime::Seconds() + 2;
	while (Total < Frame.Num())
	{
		int32 Sent = 0;
		if (Client.Socket->Send(Frame.GetData() + Total, Frame.Num() - Total, Sent) && Sent > 0) { Total += Sent; continue; }
		if (FPlatformTime::Seconds() > Deadline) return false;
		FPlatformProcess::Sleep(0.001f);
	}
	return true;
}

void FCubeWebSocketServer::Send(int32 Id, const FString& Text)
{
	FClient* Client = Clients.Find(Id);
	if (!Client || !Client->bUpgraded) return;
	const FTCHARToUTF8 Utf8(*Text);
	TArray<uint8> Payload;
	Payload.Append((const uint8*)Utf8.Get(), Utf8.Length());
	if (!SendFrame(*Client, 0x1, Payload)) Drop(Id, true);
}

TArray<uint8> FCubeWebSocketServer::CloseAnswer(const TArray<uint8>& PeerClosePayload)
{
	if (PeerClosePayload.Num() >= 2) return { PeerClosePayload[0], PeerClosePayload[1] };
	constexpr uint16 Normal = 1000;
	return { (uint8)(Normal >> 8), (uint8)(Normal & 0xFF) };
}

void FCubeWebSocketServer::Close(int32 Id, uint16 Code, const FString& Reason)
{
	FClient* Client = Clients.Find(Id);
	if (!Client) return;
	if (Client->bUpgraded)
	{
		TArray<uint8> Payload;
		Payload.Add((uint8)(Code >> 8));
		Payload.Add((uint8)Code);
		const FTCHARToUTF8 Utf8(*Reason);
		Payload.Append((const uint8*)Utf8.Get(), Utf8.Length());
		SendFrame(*Client, 0x8, Payload);
	}
	Drop(Id, true);
}

void FCubeWebSocketServer::Drop(int32 Id, bool bTell)
{
	FClient Client;
	if (!Clients.RemoveAndCopyValue(Id, Client)) return;
	if (Client.Socket)
	{
		Client.Socket->Close();
		if (ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)) Sockets->DestroySocket(Client.Socket);
	}
	if (bTell && Client.bUpgraded) OnClosed.ExecuteIfBound(Id);
}
