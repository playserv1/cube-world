#include "GasLab/GasLabLink.h"
#include "GasLab/GasLabClock.h"
#include "Common/TcpSocketBuilder.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformTime.h"
#include "IWebSocket.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Misc/Base64.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "WebSocketsModule.h"

DEFINE_LOG_CATEGORY_STATIC(LogGasLink, Log, All);

double IGasLabLink::Delay()
{
	static const double Seconds = []()
	{
		float Ms = 0.f;
		FParse::Value(FCommandLine::Get(), TEXT("-gaslinkdelay="), Ms);
		return FMath::Max(0.0, double(Ms) / 1000.0);
	}();
	return Seconds;
}

namespace
{
	/** Frames held back by -gaslinkdelay, released in order once their time comes. */
	struct FDelayQueue
	{
		struct FItem { double Due; int32 To; TArray<uint8> Bytes; };
		TArray<FItem> Items;

		void Push(int32 To, const TArray<uint8>& Bytes) { Items.Add({ FPlatformTime::Seconds() + IGasLabLink::Delay(), To, Bytes }); }

		template <typename F>
		void Release(F&& Send)
		{
			const double Now = FPlatformTime::Seconds();
			int32 N = 0;
			while (N < Items.Num() && Items[N].Due <= Now) { Send(Items[N].To, Items[N].Bytes); N++; }
			if (N > 0) Items.RemoveAt(0, N);
		}
	};

	// ── direct: a TCP mesh between the servers ───────────────────────────────────────────────

	class FTcpLink final : public IGasLabLink
	{
	public:
		virtual ~FTcpLink() override
		{
			for (FConn& C : Conns) Close(C);
			if (Listener) { Listener->Close(); ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Listener); }
		}

		virtual FString Name() const override { return TEXT("direct"); }

		virtual void Start(int32 InRegion, const TArray<FGasLabPeer>& InPeers) override
		{
			Region = InRegion;
			Peers = InPeers;
			for (const FGasLabPeer& P : Peers)
			{
				if (P.Region == Region) MyPort = P.Port;
			}
			if (MyPort > 0)
			{
				Listener = FTcpSocketBuilder(TEXT("GasLabListen")).AsNonBlocking().AsReusable().BoundToPort(MyPort).Listening(16);
				UE_LOG(LogGasLink, Display, TEXT("direct: region %d listens on %d%s"), Region, MyPort, Listener ? TEXT("") : TEXT(" FAILED"));
			}
		}

		virtual void Send(int32 ToRegion, const TArray<uint8>& Bytes) override
		{
			if (Delay() > 0) { Delayed.Push(ToRegion, Bytes); return; }
			SendNow(ToRegion, Bytes);
		}

		virtual bool IsConnected(int32 ToRegion) const override
		{
			const FConn* C = Conns.FindByPredicate([ToRegion](const FConn& X) { return X.Region == ToRegion && X.Socket && (X.bHello || X.bDialled); });
			return C != nullptr && C->Socket->GetConnectionState() == SCS_Connected;
		}

		virtual void Tick() override
		{
			Delayed.Release([this](int32 To, const TArray<uint8>& Bytes) { SendNow(To, Bytes); });
			Accept();
			Dial();
			for (FConn& C : Conns) Pump(C);
			Conns.RemoveAll([](const FConn& C) { return C.Socket == nullptr; });
		}

	private:
		struct FConn
		{
			FSocket* Socket = nullptr;
			int32 Region = -1;
			bool bHello = false;
			bool bDialled = false;
			TArray<uint8> In, Out;
		};

		void Close(FConn& C)
		{
			if (!C.Socket) return;
			C.Socket->Close();
			ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(C.Socket);
			C.Socket = nullptr;
			if (C.Region >= 0) UE_LOG(LogGasLink, Display, TEXT("direct: region %d lost region %d"), Region, C.Region);
		}

		void Accept()
		{
			if (!Listener) return;
			bool bPending = false;
			while (Listener->HasPendingConnection(bPending) && bPending)
			{
				FSocket* S = Listener->Accept(TEXT("GasLabPeerIn"));
				if (!S) break;
				S->SetNonBlocking(true);
				S->SetNoDelay(true);
				FConn C;
				C.Socket = S;
				Conns.Add(MoveTemp(C));
			}
		}

		void Dial()
		{
			const double Now = FPlatformTime::Seconds();
			if (Now < NextDial) return;
			NextDial = Now + 1.0;
			for (const FGasLabPeer& P : Peers)
			{
				// The lower region dials; the higher one accepts. One connection per pair.
				if (P.Region <= Region) continue;
				if (Conns.ContainsByPredicate([&P](const FConn& C) { return C.Region == P.Region && C.bDialled; })) continue;
				ISocketSubsystem* Subsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
				TSharedPtr<FInternetAddr> Addr = Subsystem->GetAddressFromString(P.Host);
				if (!Addr.IsValid()) continue;
				Addr->SetPort(P.Port);
				FSocket* S = Subsystem->CreateSocket(NAME_Stream, TEXT("GasLabPeerOut"), Addr->GetProtocolType());
				if (!S) continue;
				S->SetNoDelay(true);
				S->SetNonBlocking(true);
				S->Connect(*Addr);
				FConn C;
				C.Socket = S;
				C.Region = P.Region;
				C.bDialled = true;
				// Hello: who is calling. Queued until the connection is up.
				TArray<uint8> Hello;
				Hello.Add(uint8(Region));
				Frame(C, Hello);
				Conns.Add(MoveTemp(C));
			}
		}

		void Frame(FConn& C, const TArray<uint8>& Bytes)
		{
			const uint32 Len = Bytes.Num();
			C.Out.Append(reinterpret_cast<const uint8*>(&Len), 4);
			C.Out.Append(Bytes);
		}

		void SendNow(int32 ToRegion, const TArray<uint8>& Bytes)
		{
			FConn* C = Conns.FindByPredicate([ToRegion](const FConn& X) { return X.Region == ToRegion && X.Socket; });
			if (!C) return;
			Frame(*C, Bytes);
			BytesOut += Bytes.Num() + 4;
			FramesOut++;
			Flush(*C);
		}

		void Flush(FConn& C)
		{
			if (!C.Socket || C.Out.Num() == 0) return;
			if (C.Socket->GetConnectionState() != SCS_Connected) return;
			int32 Sent = 0;
			if (C.Socket->Send(C.Out.GetData(), C.Out.Num(), Sent) && Sent > 0) C.Out.RemoveAt(0, Sent, EAllowShrinking::No);
		}

		void Pump(FConn& C)
		{
			if (!C.Socket) return;
			const ESocketConnectionState State = C.Socket->GetConnectionState();
			if (State == SCS_ConnectionError) { Close(C); return; }
			Flush(C);
			uint32 Pending = 0;
			while (C.Socket && C.Socket->HasPendingData(Pending) && Pending > 0)
			{
				const int32 Start = C.In.Num();
				C.In.AddUninitialized(Pending);
				int32 Read = 0;
				if (!C.Socket->Recv(C.In.GetData() + Start, Pending, Read)) { C.In.SetNum(Start); Close(C); return; }
				C.In.SetNum(Start + Read);
				if (Read == 0) break;
			}
			int32 Offset = 0;
			while (C.In.Num() - Offset >= 4)
			{
				uint32 Len = 0;
				FMemory::Memcpy(&Len, C.In.GetData() + Offset, 4);
				if (C.In.Num() - Offset - 4 < int32(Len)) break;
				TArray<uint8> Body(C.In.GetData() + Offset + 4, Len);
				Offset += 4 + Len;
				if (!C.bHello && !C.bDialled)
				{
					C.Region = Body.Num() > 0 ? Body[0] : -1;
					C.bHello = true;
					UE_LOG(LogGasLink, Display, TEXT("direct: region %d accepted region %d"), Region, C.Region);
					continue;
				}
				C.bHello = true;
				BytesIn += Len + 4;
				FramesIn++;
				if (OnReceive) OnReceive(C.Region, Body);
			}
			if (Offset > 0) C.In.RemoveAt(0, Offset, EAllowShrinking::No);
			// A dialled socket that never comes up is dropped and dialled again.
			if (C.Socket && C.bDialled && State == SCS_NotConnected && !C.bHello) { Close(C); }
		}

		int32 Region = -1, MyPort = 0;
		TArray<FGasLabPeer> Peers;
		FSocket* Listener = nullptr;
		TArray<FConn> Conns;
		double NextDial = 0;
		FDelayQueue Delayed;
	};

	// ── relay: a WebSocket to the mock of a platform server-to-server channel ─────────────────────

	class FRelayLink final : public IGasLabLink
	{
	public:
		explicit FRelayLink(const FString& InUrl) : Url(InUrl) {}
		virtual ~FRelayLink() override { if (Socket.IsValid()) Socket->Close(); }

		virtual FString Name() const override { return TEXT("relay"); }

		virtual void Start(int32 InRegion, const TArray<FGasLabPeer>& InPeers) override
		{
			Region = InRegion;
			FModuleManager::LoadModuleChecked<FWebSocketsModule>(TEXT("WebSockets"));
			Connect();
		}

		virtual void Send(int32 ToRegion, const TArray<uint8>& Bytes) override
		{
			if (Delay() > 0) { Delayed.Push(ToRegion, Bytes); return; }
			SendNow(ToRegion, Bytes);
		}

		virtual bool IsConnected(int32) const override { return bUp; }

		virtual void Tick() override
		{
			Delayed.Release([this](int32 To, const TArray<uint8>& Bytes) { SendNow(To, Bytes); });
			if (bReset) { bReset = false; Socket.Reset(); }
			if (!Socket.IsValid() && FPlatformTime::Seconds() > Retry) Connect();
			TArray<TPair<int32, TArray<uint8>>> Ready;
			Ready = MoveTemp(Inbox);
			Inbox.Reset();
			for (TPair<int32, TArray<uint8>>& M : Ready) if (OnReceive) OnReceive(M.Key, M.Value);
		}

	private:
		void Connect()
		{
			const FString Full = FString::Printf(TEXT("%s%sregion=%d"), *Url, Url.Contains(TEXT("?")) ? TEXT("&") : TEXT("?"), Region);
			Socket = FWebSocketsModule::Get().CreateWebSocket(Full, TEXT(""));
			Socket->OnConnected().AddLambda([this]() { bUp = true; UE_LOG(LogGasLink, Display, TEXT("relay: region %d connected to %s"), Region, *Url); });
			Socket->OnConnectionError().AddLambda([this](const FString& Error) { bUp = false; UE_LOG(LogGasLink, Warning, TEXT("relay: %s"), *Error); Drop(); });
			Socket->OnClosed().AddLambda([this](int32 Code, const FString& Reason, bool) { bUp = false; UE_LOG(LogGasLink, Warning, TEXT("relay closed %d %s"), Code, *Reason); Drop(); });
			Socket->OnMessage().AddLambda([this](const FString& Text) { Hear(Text); });
			Socket->Connect();
		}

		void Drop()
		{
			Retry = FPlatformTime::Seconds() + 1.0;
			// The socket is released on the next tick, outside its own callback.
			bReset = true;
		}

		void SendNow(int32 ToRegion, const TArray<uint8>& Bytes)
		{
			if (!Socket.IsValid() || !bUp) return;
			const FString Frame = FString::Printf(TEXT("{\"type\":\"s2s_send\",\"to\":%d,\"data\":\"%s\"}"), ToRegion, *FBase64::Encode(Bytes));
			Socket->Send(Frame);
			BytesOut += FTCHARToUTF8(*Frame).Length();
			FramesOut++;
		}

		void Hear(const FString& Text)
		{
			TSharedPtr<FJsonObject> Json;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
			if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid()) return;
			if (Json->GetStringField(TEXT("type")) != TEXT("s2s_message")) return;
			TArray<uint8> Bytes;
			if (!FBase64::Decode(Json->GetStringField(TEXT("data")), Bytes)) return;
			BytesIn += FTCHARToUTF8(*Text).Length();
			FramesIn++;
			Inbox.Add({ int32(Json->GetIntegerField(TEXT("from"))), MoveTemp(Bytes) });
		}

		FString Url;
		int32 Region = -1;
		TSharedPtr<IWebSocket> Socket;
		bool bUp = false, bReset = false;
		double Retry = 0;
		TArray<TPair<int32, TArray<uint8>>> Inbox;
		FDelayQueue Delayed;
	};
}

TUniquePtr<IGasLabLink> IGasLabLink::Make()
{
	FString Relay;
	if (FParse::Value(FCommandLine::Get(), TEXT("-gasrelay="), Relay) && !Relay.IsEmpty())
	{
		return MakeUnique<FRelayLink>(Relay);
	}
	return MakeUnique<FTcpLink>();
}
