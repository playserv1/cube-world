#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Rooms/PlayServAdmissionTable.h"
#include "Rooms/PlayServRooms.h"
#include "Rooms/PlayServRoomRuntime.h"
#include "Rooms/PlayServUplinkClient.h"
#include "Rooms/PlayServUplinkTransport.h"
#include "GameFramework/PlayerController.h"
#include "IWebSocket.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if !UE_BUILD_SHIPPING

/**
 * A scripted uplink socket: records what the client sends and lets a test play the platform's
 * side (connect, frames, closes) without a network. One instance per connection attempt.
 */
class FPlayServFakeUplinkTransport : public FPlayServUplinkTransport, public TSharedFromThis<FPlayServFakeUplinkTransport>
{
public:
	bool bConnectCalled = false;
	bool bConnected = false;
	int32 CloseCode = 0;
	FString CloseReason;
	TArray<FString> Sent;

	virtual void Connect() override
	{
		bConnectCalled = true;
	}

	virtual void Close(int32 Code, const FString& Reason) override
	{
		bConnected = false;
		CloseCode = Code;
		CloseReason = Reason;
	}

	virtual bool IsConnected() const override
	{
		return bConnected;
	}

	virtual bool Send(const FString& Text) override
	{
		if (!bConnected)
		{
			return false;
		}
		Sent.Add(Text);
		return true;
	}

	void SimulateConnected()
	{
		bConnected = true;
		OnConnected.ExecuteIfBound();
	}

	void SimulateMessage(const FString& Message)
	{
		OnMessage.ExecuteIfBound(Message);
	}

	void SimulateClosed(int32 Code, const FString& Reason)
	{
		bConnected = false;
		OnClosed.ExecuteIfBound(Code, Reason, true);
	}

	static TSharedPtr<FJsonObject> ParseJson(const FString& Text)
	{
		TSharedPtr<FJsonObject> Object;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		FJsonSerializer::Deserialize(Reader, Object);
		return Object;
	}

	TSharedPtr<FJsonObject> SentJson(int32 Index) const
	{
		return Sent.IsValidIndex(Index) ? ParseJson(Sent[Index]) : nullptr;
	}

	TSharedPtr<FJsonObject> LastSentJson() const
	{
		return Sent.Num() > 0 ? ParseJson(Sent.Last()) : nullptr;
	}

	/** The last sent frame of the given `type`, or null. */
	TSharedPtr<FJsonObject> LastSentOfType(const FString& Type) const
	{
		for (int32 Index = Sent.Num() - 1; Index >= 0; --Index)
		{
			TSharedPtr<FJsonObject> Frame = ParseJson(Sent[Index]);
			FString FrameType;
			if (Frame.IsValid() && Frame->TryGetStringField(TEXT("type"), FrameType) && FrameType == Type)
			{
				return Frame;
			}
		}
		return nullptr;
	}

	int32 CountSentOfType(const FString& Type) const
	{
		int32 Count = 0;
		for (const FString& Text : Sent)
		{
			TSharedPtr<FJsonObject> Frame = ParseJson(Text);
			FString FrameType;
			if (Frame.IsValid() && Frame->TryGetStringField(TEXT("type"), FrameType) && FrameType == Type)
			{
				++Count;
			}
		}
		return Count;
	}
};

/** Hands out fake transports and remembers every one, so a test can reach the current socket. */
struct FPlayServFakeUplinkFactory
{
	TArray<TSharedPtr<FPlayServFakeUplinkTransport>> Created;
	FString LastUrl;
	FString LastCredential;

	FPlayServUplinkTransportFactory Make()
	{
		return [this](const FString& Url, const FString& Credential) -> TSharedPtr<FPlayServUplinkTransport>
		{
			LastUrl = Url;
			LastCredential = Credential;
			TSharedPtr<FPlayServFakeUplinkTransport> Transport = MakeShared<FPlayServFakeUplinkTransport>();
			Created.Add(Transport);
			return Transport;
		};
	}

	TSharedPtr<FPlayServFakeUplinkTransport> Current() const
	{
		return Created.Num() > 0 ? Created.Last() : nullptr;
	}
};

/**
 * The engine's WebSocket as the live uplink transport sees it, scripted. Close() starts the
 * closing handshake and IsConnected() stays true until the peer answers — which it never does
 * here, exactly the window in which the engine's socket warns `Close: Already closing`.
 */
class FPlayServScriptedWebSocket : public IWebSocket
{
public:
	int32 CloseCalls = 0;
	bool bOpen = false;

	virtual void Connect() override
	{
		bOpen = true;
	}

	virtual void Close(int32 Code, const FString& Reason) override
	{
		++CloseCalls;
	}

	virtual bool IsConnected() override
	{
		return bOpen;
	}

	virtual void Send(const FString& Data) override
	{
	}

	virtual void Send(const void* Data, SIZE_T Size, bool bIsBinary) override
	{
	}

	virtual void SetTextMessageMemoryLimit(uint64 TextMessageMemoryLimit) override
	{
	}

	virtual FWebSocketConnectedEvent& OnConnected() override
	{
		return ConnectedEvent;
	}

	virtual FWebSocketConnectionErrorEvent& OnConnectionError() override
	{
		return ConnectionErrorEvent;
	}

	virtual FWebSocketClosedEvent& OnClosed() override
	{
		return ClosedEvent;
	}

	virtual FWebSocketMessageEvent& OnMessage() override
	{
		return MessageEvent;
	}

	virtual FWebSocketBinaryMessageEvent& OnBinaryMessage() override
	{
		return BinaryMessageEvent;
	}

	virtual FWebSocketRawMessageEvent& OnRawMessage() override
	{
		return RawMessageEvent;
	}

	virtual FWebSocketMessageSentEvent& OnMessageSent() override
	{
		return MessageSentEvent;
	}

private:
	FWebSocketConnectedEvent ConnectedEvent;
	FWebSocketConnectionErrorEvent ConnectionErrorEvent;
	FWebSocketClosedEvent ClosedEvent;
	FWebSocketMessageEvent MessageEvent;
	FWebSocketBinaryMessageEvent BinaryMessageEvent;
	FWebSocketRawMessageEvent RawMessageEvent;
	FWebSocketMessageSentEvent MessageSentEvent;
};

/** Friend-based access to the live uplink transport, so its close path runs on a scripted engine socket. */
class FPlayServUplinkTransportTestAccess
{
public:
	/** The live transport, over this socket instead of one the WebSockets module creates. */
	static TSharedPtr<FPlayServUplinkTransport> MakeLiveTransport(const FString& Url, const FString& Credential, TSharedPtr<IWebSocket> Socket)
	{
		TSharedPtr<FPlayServWebSocketUplinkTransport> Transport = MakeShared<FPlayServWebSocketUplinkTransport>(Url, Credential);
		Transport->Socket = MoveTemp(Socket);
		return Transport;
	}
};

/** A hello ack as the platform sends it. Empty RoomConfigJson writes `"room_config":null`. */
inline FString MakeHelloAck(const FString& Admission, const FString& RoomConfigJson, int32 ExpiresIn = 3600)
{
	return FString::Printf(TEXT("{\"type\":\"uplink_hello_ack\",\"session_token\":\"eyJ.test.token\",\"expires_in\":%d,\"admission\":\"%s\",\"room_config\":%s}"),
		ExpiresIn, *Admission, RoomConfigJson.IsEmpty() ? TEXT("null") : *RoomConfigJson);
}

inline FString DefaultRoomConfigJson()
{
	return TEXT("{\"capacity\":8,\"reservation_ttl_seconds\":10,\"room_lifetime_seconds\":3600,\"room_idle_timeout_seconds\":120,\"max_rooms\":20,\"version\":4}");
}

/**
 * Friend-based access to UPlayServRooms's private members, so the module's uplink,
 * presence and admission logic runs against a scripted socket and a fake clock, with no
 * heartbeat leaving the process. Lives in the test project only.
 */
class FPlayServRoomsTestAccess
{
public:
	/** Bring the module up on a fake socket without touching settings, the auth session or the ticker. */
	static void BeginWithUplink(UPlayServRooms* Server, FPlayServUplinkTransportFactory Factory, TFunction<double()> Clock, const FString& RoomTypeSlug)
	{
		ForgetRooms(Server);
		Server->StopHosting();
		Server->TransportFactory = MoveTemp(Factory);
		Server->Clock = MoveTemp(Clock);
		Server->RoomDefaultSlug = RoomTypeSlug;
		Server->InstanceId = TEXT("test-instance-0001");
		Server->bHosting = true;
		Server->OpenUplink();
	}

	/** Undo BeginWithUplink: stop, and restore the live transport, the real clock and the default policy. */
	static void End(UPlayServRooms* Server)
	{
		ForgetRooms(Server);
		Server->StopHosting();
		Server->TransportFactory = FPlayServWebSocketUplinkTransport::MakeFactory();
		Server->Clock = []()
		{
			return FPlatformTime::Seconds();
		};
		Server->ConnectionUrlOf = &UPlayServRooms::RequestUrlOf;
		Server->LivePlayerControllers = &UPlayServRooms::HostedPlayerControllers;
		Server->LoadStartedAt = 0.0;
		Server->bAdmissionFailOpen = false;
		Server->RoomDefaultSlug.Empty();
		Server->InstanceId.Empty();
		Server->Launch = UPlayServRooms::ReadLaunch();
	}

	/** Stand in for the engine's view of the connections: the travel URL each controller's connection carries, and the controllers the server's worlds hold. End() restores both. */
	static void SetConnections(UPlayServRooms* Server, TFunction<FString(const APlayerController*)> UrlOf, TFunction<TArray<const APlayerController*>()> Controllers)
	{
		Server->ConnectionUrlOf = MoveTemp(UrlOf);
		Server->LivePlayerControllers = MoveTemp(Controllers);
	}

	/** The engine's PreLoadMap: a non-seamless travel started loading the next map. */
	static void MapLoadStarted(UPlayServRooms* Server)
	{
		Server->HandlePreLoadMap(FString());
	}

	/** The engine's PostLoadMapWithWorld. */
	static void MapLoaded(UPlayServRooms* Server)
	{
		Server->HandlePostLoadMap(nullptr);
	}

	/** The engine's PostLogin, with the URL the connection would have carried. */
	static void PostLogin(UPlayServRooms* Server, const APlayerController* Player, const FString& RequestUrl)
	{
		Server->AdmitLogin(Player, RequestUrl);
	}

	/** The engine's Logout. */
	static void Logout(UPlayServRooms* Server, const APlayerController* Player)
	{
		Server->DropLogin(Player);
	}

	static void Tick(UPlayServRooms* Server, double Now)
	{
		if (Server->Uplink.IsValid())
		{
			Server->Uplink->Tick(Now);
		}
		Server->TickRooms(Now);
	}

	/** Register a room locally as if its first heartbeat had been answered; the next beat is pushed far out so no HTTP leaves. */
	static void AddRegisteredRoom(UPlayServRooms* Server, const FPlayServRoomSnapshot& Snapshot, double Now)
	{
		TSharedPtr<FPlayServRoomRuntime> Room = MakeShared<FPlayServRoomRuntime>();
		Room->Snapshot = Snapshot;
		Room->bRegistered = true;
		Room->StartedAt = Now;
		Room->IdleSince = Now;
		Room->NextBeatAt = Now + 1.0e9;
		Room->RosterSentForGeneration = Server->UplinkGeneration;
		Server->Rooms.Add(Snapshot.RoomName, Room);
	}

	/** Feed a heartbeat answer as if the platform had sent it. */
	static void DeliverHeartbeatAnswer(UPlayServRooms* Server, const FString& RoomName, int32 Status, const FString& ProblemCode, const FString& Json)
	{
		TSharedPtr<FJsonObject> Body = FPlayServFakeUplinkTransport::ParseJson(Json);
		const bool bSuccess = Status >= 200 && Status < 300;
		FPlayServError Error = bSuccess ? FPlayServError::Success() : FPlayServError::Make(EPlayServErrorCode::Unknown, FString::Printf(TEXT("HTTP %d %s"), Status, *ProblemCode));
		Error.ProblemCode = bSuccess ? FString() : ProblemCode;
		Server->HandleHeartbeatAnswer(RoomName, bSuccess, Status, ProblemCode, Body, Error);
	}

	static bool RosterContains(const UPlayServRooms* Server, const FString& RoomName, const FString& PlayerId)
	{
		const TSharedPtr<FPlayServRoomRuntime>* Room = Server->Rooms.Find(RoomName);
		return Room != nullptr && (*Room)->Roster.Contains(PlayerId);
	}

	static bool IsParked(const UPlayServRooms* Server, const FString& RoomName, const FString& PlayerId)
	{
		const TSharedPtr<FPlayServRoomRuntime>* Room = Server->Rooms.Find(RoomName);
		return Room != nullptr && (*Room)->Parked.Contains(PlayerId);
	}

	static int32 TicketCount(const UPlayServRooms* Server)
	{
		return Server->Tickets.IsValid() ? Server->Tickets->Num() : 0;
	}

	static int32 UplinkGeneration(const UPlayServRooms* Server)
	{
		return Server->UplinkGeneration;
	}

	static void SetAdmissionFailOpen(UPlayServRooms* Server, bool bFailOpen)
	{
		Server->bAdmissionFailOpen = bFailOpen;
	}

	/** How many accepted verdicts are still waiting for the login they belong to. */
	static int32 VerifiedCount(const UPlayServRooms* Server)
	{
		return Server->Verified.Num();
	}

	/** Release a ticket the way a room close or the expiry sweep does — the frame is internal now. */
	static bool ReleaseTicket(UPlayServRooms* Server, const FString& Token, const FString& Reason, const FString& Detail)
	{
		return Server->ReleaseTicket(Token, Reason, Detail);
	}

	/** The address a registered room actually carries, read back off the module's own runtime. */
	static FPlayServRoomConnect RegisteredConnect(const UPlayServRooms* Server, const FString& RoomName)
	{
		const TSharedPtr<FPlayServRoomRuntime>* Room = Server->Rooms.Find(RoomName);
		return Room ? (*Room)->Snapshot.Connect : FPlayServRoomConnect();
	}

	static FString ResolveCredential(const FString& DeploymentToken, const FString& ServerKey)
	{
		return UPlayServRooms::ResolveCredential(DeploymentToken, ServerKey);
	}

	static FString ResolveInstanceId(const FString& DeploymentToken)
	{
		return UPlayServRooms::ResolveInstanceId(DeploymentToken);
	}

	/** RequestNewRoom with the room type named — the public call always takes the project's own. */
	static void RequestNewRoomOfType(UPlayServRooms* Rooms, const FString& Slug, const TMap<FString, FString>& Attributes, FPlayServRequestNewRoomCallback Callback)
	{
		Rooms->RequestNewRoomOfType(Slug, Attributes, MoveTemp(Callback));
	}

	/** The room a launch with these values describes — the pure rule behind StartRoomPlayServHosted. */
	static FPlayServError SnapshotFromLaunch(const FString& RoomName, int32 ListenPort, const FString& PublicHost, const TMap<FString, FString>& Attributes, FPlayServRoomSnapshot& OutSnapshot)
	{
		return UPlayServRooms::SnapshotFromLaunch(MakeLaunch(RoomName, ListenPort, PublicHost, Attributes), OutSnapshot);
	}

	/** Stand in for what settings read out of a platform launch at startup. End() restores the real one. */
	static void SetLaunch(UPlayServRooms* Server, const FString& RoomName, int32 ListenPort, const FString& PublicHost, const TMap<FString, FString>& Attributes)
	{
		Server->Launch = MakeLaunch(RoomName, ListenPort, PublicHost, Attributes);
	}

private:
	/**
	 * A test's rooms are the test's: dropped locally, with no `:close`. StopHosting closes a
	 * registered room over HTTP and keeps it until the answer, which lands in whichever test runs
	 * then — and removes that test's room if it has the same name.
	 */
	static void ForgetRooms(UPlayServRooms* Server)
	{
		Server->Rooms.Reset();
		Server->PendingRoomStarts.Reset();
	}

	static UPlayServRooms::FLaunch MakeLaunch(const FString& RoomName, int32 ListenPort, const FString& PublicHost, const TMap<FString, FString>& Attributes)
	{
		UPlayServRooms::FLaunch Launch;
		Launch.RoomName = RoomName;
		Launch.ListenPort = ListenPort;
		Launch.PublicHost = PublicHost;
		Launch.Attributes = Attributes;
		return Launch;
	}
};

#endif // !UE_BUILD_SHIPPING
