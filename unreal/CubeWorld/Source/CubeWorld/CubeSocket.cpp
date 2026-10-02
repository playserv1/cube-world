#include "CubeSocket.h"
#include "CubeWorld.h"
#include "Async/Async.h"
#include "Misc/Base64.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "SocketSubsystem.h"
#include "SslModule.h"
#include "Interfaces/ISslCertificateManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

// OpenSSL names a type UI, which collides with the engine namespace of that name.
#define UI UI_ST
THIRD_PARTY_INCLUDES_START
#include <openssl/ssl.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
THIRD_PARTY_INCLUDES_END
#undef UI

static bool CubeSocketVerbose() { static const bool bVerbose = FParse::Param(FCommandLine::Get(), TEXT("logframes")); return bVerbose; }

namespace
{
	/** A text frame this large is parsed on a worker (a C# welcome on dev was 2.4 MB on 2026-10-02). */
	constexpr int32 ParseOnWorkerBytes = 32 * 1024;

	void ParseText(const TArray<uint8>& Payload, const TFunction<void(FCubeSocketFrame&)>& Decode, FCubeSocketFrame& Out)
	{
		// The converter does not null-terminate: take exactly the converted length.
		const FUTF8ToTCHAR Converted((const ANSICHAR*)Payload.GetData(), Payload.Num());
		const FString Text(Converted.Length(), Converted.Get());
		TSharedPtr<FJsonObject> Json;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) || !Json.IsValid()) return;
		Out.Json = Json;
		if (Decode) Decode(Out);
	}

	FString LastOpenSslError()
	{
		char Buffer[256] = { 0 };
		const unsigned long Code = ERR_get_error();
		if (Code == 0) return TEXT("no detail");
		ERR_error_string_n(Code, Buffer, sizeof(Buffer));
		return FString(ANSI_TO_TCHAR(Buffer));
	}
}

FCubeSocket::FCubeSocket(const FString& InHost, int32 InPort, bool bInSecure, const FString& InPath)
	: Host(InHost), Path(InPath.IsEmpty() ? TEXT("/") : InPath), Port(InPort), bSecure(bInSecure)
{
}

FCubeSocket::~FCubeSocket()
{
	if (Ticker.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(Ticker);
	FreeConnection();
}

void FCubeSocket::FreeConnection()
{
	if (Bio) { BIO_free_all((BIO*)Bio); Bio = nullptr; }
	if (SslContext) { SSL_CTX_free((SSL_CTX*)SslContext); SslContext = nullptr; }
}

void FCubeSocket::Connect()
{
	if (State != EState::Idle) return;
	State = EState::Connecting;
	// The socket layer must be up (Winsock on Windows) and the SSL module loaded before OpenSSL is used.
	ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	FSslModule::Get();
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

// Runs on the worker: connect (and handshake TLS), send the upgrade request and read the answer.
bool FCubeSocket::DoConnect(FString& OutError)
{
	const FTCHARToUTF8 HostUtf8(*Host);
	const FString HostPort = FString::Printf(TEXT("%s:%d"), *Host, Port);
	const FTCHARToUTF8 HostPortUtf8(*HostPort);
	BIO* NewBio = nullptr;
	SSL* Ssl = nullptr;

	if (bSecure)
	{
		SSL_CTX* Ctx = SSL_CTX_new(TLS_client_method());
		if (!Ctx) { OutError = TEXT("no TLS context"); return false; }
		FSslModule::Get().GetCertificateManager().AddCertificatesToSslContext(Ctx);
		SSL_CTX_set_verify(Ctx, SSL_VERIFY_PEER, nullptr);
		SSL_CTX_set_min_proto_version(Ctx, TLS1_2_VERSION);
		SslContext = Ctx;
		NewBio = BIO_new_ssl_connect(Ctx);
		if (!NewBio) { OutError = TEXT("no TLS connection"); return false; }
		BIO_get_ssl(NewBio, &Ssl);
		SSL_set_mode(Ssl, SSL_MODE_AUTO_RETRY);
		SSL_set_tlsext_host_name(Ssl, HostUtf8.Get());
		SSL_set1_host(Ssl, HostUtf8.Get());
	}
	else
	{
		NewBio = BIO_new(BIO_s_connect());
		if (!NewBio) { OutError = TEXT("no connection"); return false; }
	}
	BIO_set_conn_hostname(NewBio, HostPortUtf8.Get());
	BIO_set_nbio(NewBio, 0);

	if (BIO_do_connect(NewBio) <= 0)
	{
		OutError = FString::Printf(TEXT("connect to %s failed: %s"), *HostPort, *LastOpenSslError());
		BIO_free_all(NewBio);
		return false;
	}
	if (bSecure)
	{
		if (BIO_do_handshake(NewBio) <= 0)
		{
			OutError = FString::Printf(TEXT("TLS handshake with %s failed: %s"), *Host, *LastOpenSslError());
			BIO_free_all(NewBio);
			return false;
		}
		const long Verify = SSL_get_verify_result(Ssl);
		if (Verify != X509_V_OK)
		{
			OutError = FString::Printf(TEXT("certificate of %s rejected: %s"), *Host, ANSI_TO_TCHAR(X509_verify_cert_error_string(Verify)));
			BIO_free_all(NewBio);
			return false;
		}
	}

	uint8 KeyBytes[16];
	for (uint8& B : KeyBytes) B = (uint8)FMath::RandRange(0, 255);
	const FString Key = FBase64::Encode(KeyBytes, 16);
	const FString Request = FString::Printf(TEXT("GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n"), *Path, *HostPort, *Key);
	const FTCHARToUTF8 Utf8(*Request);
	if (BIO_write(NewBio, Utf8.Get(), Utf8.Length()) <= 0) { OutError = TEXT("handshake send failed"); BIO_free_all(NewBio); return false; }

	TArray<uint8> Answer;
	const double Deadline = FPlatformTime::Seconds() + 10;
	while (FPlatformTime::Seconds() < Deadline)
	{
		uint8 Buffer[4096];
		const int32 Read = BIO_read(NewBio, Buffer, sizeof(Buffer));
		if (Read <= 0)
		{
			if (BIO_should_retry(NewBio)) continue;
			OutError = TEXT("handshake: connection closed");
			BIO_free_all(NewBio);
			return false;
		}
		Answer.Append(Buffer, Read);
		const FUTF8ToTCHAR AnswerConverted((const ANSICHAR*)Answer.GetData(), Answer.Num());
		const FString Text(AnswerConverted.Length(), AnswerConverted.Get());
		const int32 End = Text.Find(TEXT("\r\n\r\n"));
		if (End < 0) continue;
		if (!Text.StartsWith(TEXT("HTTP/1.1 101")))
		{
			OutError = FString::Printf(TEXT("handshake refused: %s"), *Text.Left(Text.Find(TEXT("\r\n"))));
			BIO_free_all(NewBio);
			return false;
		}
		// Whatever followed the headers is already a frame.
		const FTCHARToUTF8 HeadUtf8(*Text.Left(End + 4));
		const int32 HeadBytes = HeadUtf8.Length();
		FScopeLock Guard(&Lock);
		if (Answer.Num() > HeadBytes) Incoming.Append(Answer.GetData() + HeadBytes, Answer.Num() - HeadBytes);
		// BIO_set_nbio only acts before the connect; from here the socket itself is switched to non-blocking,
		// so the reads on the game thread return at once when nothing is waiting.
		int FileDescriptor = -1;
		BIO_get_fd(NewBio, &FileDescriptor);
		if (FileDescriptor >= 0) BIO_socket_nbio(FileDescriptor, 1);
		Bio = NewBio;
		return true;
	}
	OutError = TEXT("handshake timed out");
	BIO_free_all(NewBio);
	return false;
}

bool FCubeSocket::Tick(float)
{
	const double TickStart = FPlatformTime::Seconds();
	ON_SCOPE_EXIT { const double Ms = (FPlatformTime::Seconds() - TickStart) * 1000; if (Ms > 20 && CubeSocketVerbose()) UE_LOG(LogCubeWorld, Log, TEXT("slow: socket tick %.0f ms"), Ms); };
	if (State == EState::Connecting)
	{
		if (!bWorkerDone) return true;
		FString Error;
		{ FScopeLock Guard(&Lock); Error = WorkerError; }
		if (!Error.IsEmpty()) { Fail(Error); return false; }
		State = EState::Open;
		OnConnected.Broadcast();
	}
	if (State != EState::Open || !Bio) return State == EState::Open;

	// Non-blocking reads until the connection has nothing more; 0 with no retry is the peer closing.
	for (int32 Rounds = 0; Rounds < 64; Rounds++)
	{
		uint8 Buffer[16384];
		const int32 Read = BIO_read((BIO*)Bio, Buffer, sizeof(Buffer));
		if (Read > 0)
		{
			if (CubeSocketVerbose()) UE_LOG(LogCubeWorld, Log, TEXT("socket: read %d"), Read);
			Incoming.Append(Buffer, Read);
			continue;
		}
		if (BIO_should_retry((BIO*)Bio)) break;
		ReadFrames();
		Deliver(true);   // what came before the end goes out first
		if (State == EState::Open)
		{
			State = EState::Closed;
			OnClosed.Broadcast(Read == 0 ? TEXT("connection closed by the server") : FString::Printf(TEXT("connection lost: %s"), *LastOpenSslError()));
		}
		return false;
	}
	ReadFrames();
	Deliver(false);
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
			ReceiveText(MoveTemp(Payload));
			if (State != EState::Open) return;
		}
		else if (Opcode == 0x9) SendFrame(0xA, Payload);
		else if (Opcode == 0x8)
		{
			FString Reason;
			if (Payload.Num() > 2) { const FUTF8ToTCHAR ReasonConverted((const ANSICHAR*)Payload.GetData() + 2, Payload.Num() - 2); Reason = FString(ReasonConverted.Length(), ReasonConverted.Get()); }
			Deliver(true);   // what came before the close goes out first
			if (State != EState::Open) return;
			State = EState::Closed;
			OnClosed.Broadcast(Reason);
			return;
		}
	}
}

void FCubeSocket::ReceiveText(TArray<uint8> Payload)
{
	if (CubeSocketVerbose())
	{
		const FUTF8ToTCHAR Head((const ANSICHAR*)Payload.GetData(), FMath::Min(Payload.Num(), 600));
		UE_LOG(LogCubeWorld, Log, TEXT("socket: text %s"), *FString(Head.Length(), Head.Get()));
	}
	if (Pending.Num() == 0 && Payload.Num() < ParseOnWorkerBytes)
	{
		FCubeSocketFrame Frame;
		ParseText(Payload, Decode, Frame);
		if (Frame.Json.IsValid()) OnFrame.Broadcast(Frame);
		return;
	}
	// A large frame, or any frame behind one, waits for its parse on a worker and goes out in turn.
	FPending Item;
	Item.Frame = MakeShared<FCubeSocketFrame, ESPMode::ThreadSafe>();
	Item.Parse = UE::Tasks::Launch(UE_SOURCE_LOCATION, [Frame = Item.Frame, Payload = MoveTemp(Payload), Decode = Decode]() { ParseText(Payload, Decode, *Frame); });
	Pending.Add(MoveTemp(Item));
}

void FCubeSocket::Deliver(bool bWait)
{
	while (Pending.Num() > 0 && State == EState::Open)
	{
		if (!Pending[0].Parse.IsCompleted())
		{
			if (!bWait) return;
			Pending[0].Parse.Wait();
		}
		const TSharedPtr<FCubeSocketFrame, ESPMode::ThreadSafe> Frame = Pending[0].Frame;
		Pending.RemoveAt(0);
		if (Frame->Json.IsValid()) OnFrame.Broadcast(*Frame);
	}
}

void FCubeSocket::SendFrame(uint8 Opcode, const TArray<uint8>& Payload)
{
	const double SendStart = FPlatformTime::Seconds();
	ON_SCOPE_EXIT { const double Ms = (FPlatformTime::Seconds() - SendStart) * 1000; if (Ms > 20 && CubeSocketVerbose()) UE_LOG(LogCubeWorld, Log, TEXT("slow: send %.0f ms"), Ms); };
	if (!Bio || State != EState::Open) return;
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
	const double Deadline = FPlatformTime::Seconds() + 2;
	while (Total < Frame.Num())
	{
		const int32 Sent = BIO_write((BIO*)Bio, Frame.GetData() + Total, Frame.Num() - Total);
		if (Sent > 0) { Total += Sent; continue; }
		if (!BIO_should_retry((BIO*)Bio) || FPlatformTime::Seconds() > Deadline) { Fail(TEXT("send failed")); return; }
		FPlatformProcess::Sleep(0.001f);
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
	FreeConnection();
}

void FCubeSocket::Fail(const FString& Reason)
{
	State = EState::Closed;
	OnError.Broadcast(Reason);
}
