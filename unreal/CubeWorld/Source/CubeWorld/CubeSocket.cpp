#include "CubeSocket.h"
#include "CubeWorld.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
#include "Async/Async.h"
#include "Misc/Base64.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

static bool CubeSocketVerbose() { static const bool bVerbose = FParse::Param(FCommandLine::Get(), TEXT("logframes")); return bVerbose; }

FCubeSocket::FCubeSocket(const FString& InHost, int32 InPort, const FString& InPath)
	: Host(InHost), Path(InPath.IsEmpty() ? TEXT("/") : InPath), Port(InPort)
{
}

FCubeSocket::~FCubeSocket()
{
	if (Ticker.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(Ticker);
	if (Socket)
	{
		Socket->Close();
		ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
	}
}

void FCubeSocket::Connect()
{
	if (State != EState::Idle) return;
	State = EState::Connecting;
	TWeakPtr<FCubeSocket> Weak = AsShared();
	Worker = Async(EAsyncExecution::Thread, [Weak]()
	{
		if (const TSharedPtr<FCubeSocket> Self = Weak.Pin())
		{
			FString Error;
			const bool bOk = Self->DoConnect(Error);
			FScopeLock Guard(&Self->Lock);
			Self->WorkerError = bOk ? FString() : Error;
			Self->bWorkerDone = true;
		}
	});
	Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateSP(this, &FCubeSocket::Tick), 0.f);
}

// Runs on the worker: resolve, connect, send the upgrade request and read the answer.
bool FCubeSocket::DoConnect(FString& OutError)
{
	ISocketSubsystem* Subsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	TSharedPtr<FInternetAddr> Address;
	bool bValid = false;
	Address = Subsystem->CreateInternetAddr();
	Address->SetIp(*Host, bValid);
	if (!bValid)
	{
		const FAddressInfoResult Info = Subsystem->GetAddressInfo(*Host, nullptr, EAddressInfoFlags::Default, NAME_None);
		if (Info.Results.Num() == 0) { OutError = FString::Printf(TEXT("cannot resolve %s"), *Host); return false; }
		Address = Info.Results[0].Address;
	}
	Address->SetPort(Port);

	FSocket* NewSocket = Subsystem->CreateSocket(NAME_Stream, TEXT("cubeworld"), Address->GetProtocolType());
	if (!NewSocket) { OutError = TEXT("no socket"); return false; }
	NewSocket->SetNoDelay(true);
	if (!NewSocket->Connect(*Address))
	{
		OutError = FString::Printf(TEXT("connect to %s:%d failed"), *Host, Port);
		Subsystem->DestroySocket(NewSocket);
		return false;
	}

	uint8 KeyBytes[16];
	for (uint8& B : KeyBytes) B = (uint8)FMath::RandRange(0, 255);
	const FString Key = FBase64::Encode(KeyBytes, 16);
	const FString Request = FString::Printf(TEXT("GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n"), *Path, *Host, Port, *Key);
	const FTCHARToUTF8 Utf8(*Request);
	int32 Sent = 0;
	if (!NewSocket->Send((const uint8*)Utf8.Get(), Utf8.Length(), Sent)) { OutError = TEXT("handshake send failed"); Subsystem->DestroySocket(NewSocket); return false; }

	TArray<uint8> Answer;
	const double Deadline = FPlatformTime::Seconds() + 10;
	while (FPlatformTime::Seconds() < Deadline)
	{
		uint8 Buffer[4096];
		int32 Read = 0;
		if (!NewSocket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromMilliseconds(200))) continue;
		if (!NewSocket->Recv(Buffer, sizeof(Buffer), Read) || Read <= 0) { OutError = TEXT("handshake: connection closed"); Subsystem->DestroySocket(NewSocket); return false; }
		Answer.Append(Buffer, Read);
		const FUTF8ToTCHAR AnswerConverted((const ANSICHAR*)Answer.GetData(), Answer.Num());
		const FString Text(AnswerConverted.Length(), AnswerConverted.Get());
		const int32 End = Text.Find(TEXT("\r\n\r\n"));
		if (End < 0) continue;
		if (!Text.StartsWith(TEXT("HTTP/1.1 101")))
		{
			OutError = FString::Printf(TEXT("handshake refused: %s"), *Text.Left(Text.Find(TEXT("\r\n"))));
			Subsystem->DestroySocket(NewSocket);
			return false;
		}
		// Whatever followed the headers is already a frame.
		const FTCHARToUTF8 HeadUtf8(*Text.Left(End + 4));
		const int32 HeadBytes = HeadUtf8.Length();
		FScopeLock Guard(&Lock);
		if (Answer.Num() > HeadBytes) Incoming.Append(Answer.GetData() + HeadBytes, Answer.Num() - HeadBytes);
		NewSocket->SetNonBlocking(true);
		Socket = NewSocket;
		return true;
	}
	OutError = TEXT("handshake timed out");
	Subsystem->DestroySocket(NewSocket);
	return false;
}

bool FCubeSocket::Tick(float)
{
	if (State == EState::Connecting)
	{
		if (!bWorkerDone) return true;
		FString Error;
		{ FScopeLock Guard(&Lock); Error = WorkerError; }
		if (!Error.IsEmpty()) { Fail(Error); return false; }
		State = EState::Open;
		OnConnected.Broadcast();
	}
	if (State != EState::Open || !Socket) return State == EState::Open;

	uint32 Pending = 0;
	while (Socket->HasPendingData(Pending) && Pending > 0)
	{
		TArray<uint8> Buffer;
		Buffer.SetNumUninitialized(FMath::Min<uint32>(Pending, 65536));
		int32 Read = 0;
		const bool bOk = Socket->Recv(Buffer.GetData(), Buffer.Num(), Read);
		if (CubeSocketVerbose()) UE_LOG(LogCubeWorld, Log, TEXT("socket: pending %u read %d ok %d"), Pending, Read, bOk);
		if (!bOk || Read <= 0) break;
		Incoming.Append(Buffer.GetData(), Read);
	}
	// A graceful close shows as a failed zero-byte peek; a would-block peek succeeds with nothing read.
	uint8 Probe = 0;
	int32 Peeked = 0;
	if (!Socket->Recv(&Probe, 1, Peeked, ESocketReceiveFlags::Peek) && Peeked == 0)
	{
		ReadFrames();
		if (State == EState::Open) { State = EState::Closed; OnClosed.Broadcast(TEXT("connection closed by the server")); }
		return false;
	}
	if (Socket->GetConnectionState() == SCS_ConnectionError) { Fail(TEXT("connection lost")); return false; }
	ReadFrames();
	return State == EState::Open;
}

void FCubeSocket::ReadFrames()
{
	while (Incoming.Num() >= 2)
	{
		const uint8 B0 = Incoming[0], B1 = Incoming[1];
		const uint8 Opcode = B0 & 0x0F;
		const bool bMasked = (B1 & 0x80) != 0;
		uint64 Length = B1 & 0x7F;
		int32 Offset = 2;
		if (Length == 126) { if (Incoming.Num() < 4) return; Length = ((uint64)Incoming[2] << 8) | Incoming[3]; Offset = 4; }
		else if (Length == 127) { if (Incoming.Num() < 10) return; Length = 0; for (int32 I = 0; I < 8; I++) Length = (Length << 8) | Incoming[2 + I]; Offset = 10; }
		if (bMasked) Offset += 4;
		if ((uint64)Incoming.Num() < Offset + Length) return;
		if (CubeSocketVerbose()) UE_LOG(LogCubeWorld, Log, TEXT("socket: frame opcode %d fin %d masked %d length %llu buffered %d"), Opcode, (B0 & 0x80) != 0, bMasked, Length, Incoming.Num());

		TArray<uint8> Payload;
		Payload.Append(Incoming.GetData() + Offset, (int32)Length);
		if (bMasked) for (int32 I = 0; I < Payload.Num(); I++) Payload[I] ^= Incoming[Offset - 4 + (I % 4)];
		Incoming.RemoveAt(0, Offset + (int32)Length, EAllowShrinking::No);

		if (Opcode == 0x1 || Opcode == 0x0)
		{
			// The converter does not null-terminate: take exactly the converted length.
			const FUTF8ToTCHAR Converted((const ANSICHAR*)Payload.GetData(), Payload.Num());
			const FString Text(Converted.Length(), Converted.Get());
			if (CubeSocketVerbose()) UE_LOG(LogCubeWorld, Log, TEXT("socket: text %s"), *Text.Left(90));
			OnMessage.Broadcast(Text);
			if (State != EState::Open) return;
		}
		else if (Opcode == 0x9) SendFrame(0xA, Payload);
		else if (Opcode == 0x8)
		{
			FString Reason;
			if (Payload.Num() > 2) { const FUTF8ToTCHAR ReasonConverted((const ANSICHAR*)Payload.GetData() + 2, Payload.Num() - 2); Reason = FString(ReasonConverted.Length(), ReasonConverted.Get()); }
			State = EState::Closed;
			OnClosed.Broadcast(Reason);
			return;
		}
	}
}

void FCubeSocket::SendFrame(uint8 Opcode, const TArray<uint8>& Payload)
{
	if (!Socket || State != EState::Open) return;
	TArray<uint8> Frame;
	Frame.Add(0x80 | Opcode);
	const int32 N = Payload.Num();
	if (N < 126) Frame.Add(0x80 | (uint8)N);
	else if (N < 65536) { Frame.Add(0x80 | 126); Frame.Add((uint8)(N >> 8)); Frame.Add((uint8)N); }
	else { Frame.Add(0x80 | 127); for (int32 I = 7; I >= 0; I--) Frame.Add((uint8)(((uint64)N >> (I * 8)) & 0xFF)); }
	uint8 Mask[4];
	for (uint8& M : Mask) { M = (uint8)FMath::RandRange(0, 255); Frame.Add(M); }
	const int32 Start = Frame.Num();
	Frame.AddUninitialized(N);
	for (int32 I = 0; I < N; I++) Frame[Start + I] = Payload[I] ^ Mask[I % 4];

	int32 Total = 0;
	while (Total < Frame.Num())
	{
		int32 Sent = 0;
		if (!Socket->Send(Frame.GetData() + Total, Frame.Num() - Total, Sent))
		{
			if (Socket->GetConnectionState() == SCS_ConnectionError) { Fail(TEXT("send failed")); return; }
			Socket->Wait(ESocketWaitConditions::WaitForWrite, FTimespan::FromMilliseconds(50));
			continue;
		}
		Total += Sent;
	}
}

void FCubeSocket::Send(const FString& Text)
{
	const FTCHARToUTF8 Utf8(*Text);
	TArray<uint8> Payload;
	Payload.Append((const uint8*)Utf8.Get(), Utf8.Length());
	SendFrame(0x1, Payload);
}

void FCubeSocket::Close()
{
	if (State == EState::Open) SendFrame(0x8, TArray<uint8>());
	State = EState::Closed;
	if (Ticker.IsValid()) { FTSTicker::GetCoreTicker().RemoveTicker(Ticker); Ticker.Reset(); }
	if (Socket) Socket->Close();
}

void FCubeSocket::Fail(const FString& Reason)
{
	State = EState::Closed;
	OnError.Broadcast(Reason);
}
