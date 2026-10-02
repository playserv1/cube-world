#include "PlayServRoomsEventSpy.h"
#include "PlayServRoomsTestHelpers.h"
#include "PlayServTestAccess.h"
#include "PlayServTestCommon.h"
#include "Core/PlayServSettings.h"
#include "Core/PlayServSubsystem.h"
#include "Rooms/PlayServAdmissionTable.h"
#include "Rooms/PlayServLogForwarder.h"
#include "Rooms/PlayServRoomRuntime.h"
#include "Core/PlayServHttp.h"
#include "Rooms/PlayServRoomsClientWire.h"
#include "Rooms/PlayServRoomsPaths.h"
#include "Rooms/PlayServRoomsValidation.h"
#include "Rooms/PlayServRoomsWire.h"
#include "Rooms/PlayServRosterHash.h"
#include "Rooms/PlayServUplinkClient.h"
#include "GameFramework/PlayerController.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"

#if !UE_BUILD_SHIPPING

namespace
{
	/** A JWT with this payload, unpadded like the platform's; the signature is never checked. */
	FString MakeDeploymentToken(const FString& PayloadJson)
	{
		FString Header = FBase64::Encode(FString(TEXT("{\"alg\":\"HS256\",\"typ\":\"JWT\"}")), EBase64Mode::UrlSafe);
		FString Payload = FBase64::Encode(PayloadJson, EBase64Mode::UrlSafe);
		Header.ReplaceInline(TEXT("="), TEXT(""));
		Payload.ReplaceInline(TEXT("="), TEXT(""));
		return FString::Printf(TEXT("%s.%s.c2lnbmF0dXJl"), *Header, *Payload);
	}

	/** A launch as the platform's launcher hands it to one process. */
	struct FTestLaunch
	{
		FString RoomName = TEXT("r-0123456789abcdef");
		int32 ListenPort = 7777;
		FString PublicHost = TEXT("203.0.113.40");
		TMap<FString, FString> Attributes;

		FTestLaunch()
		{
			Attributes.Add(TEXT("map"), TEXT("BlobArenaMap"));
			Attributes.Add(TEXT("name"), TEXT("Room of player one"));
		}
	};

	FPlayServError SnapshotOf(const FTestLaunch& Launch, FPlayServRoomSnapshot& OutSnapshot)
	{
		return FPlayServRoomsTestAccess::SnapshotFromLaunch(Launch.RoomName, Launch.ListenPort, Launch.PublicHost, Launch.Attributes, OutSnapshot);
	}

	FPlayServRoomSnapshot MakeSnapshot(const FString& RoomName)
	{
		FPlayServRoomSnapshot Snapshot;
		Snapshot.RoomName = RoomName;
		Snapshot.Capacity = 16;
		Snapshot.State = TEXT("lobby");
		Snapshot.Attributes.Add(TEXT("name"), TEXT("Blob Arena"));
		Snapshot.Attributes.Add(TEXT("map"), TEXT("arena"));
		Snapshot.Connect.Host = TEXT("203.0.113.5");
		Snapshot.Connect.Port = 7777;
		Snapshot.Connect.Transport = EPlayServRoomTransport::Udp;
		Snapshot.Region = TEXT("eu-west-1");
		return Snapshot;
	}

	struct FFakeClock
	{
		double Now = 1000.0;
		TFunction<double()> Fn()
		{
			return [this]()
			{
				return Now;
			};
		}
	};

	/**
	 * The engine's side of a hosting server's connections, scripted: the controllers its worlds hold
	 * and the travel URL each one's connection carries (UNetConnection::RequestURL).
	 */
	struct FScriptedConnections
	{
		TMap<const APlayerController*, FString> Urls;
		TArray<const APlayerController*> Live;

		void Install(UPlayServRooms* Server)
		{
			TFunction<FString(const APlayerController*)> UrlOf = [this](const APlayerController* Player)
			{
				const FString* Url = Urls.Find(Player);
				return Url != nullptr ? *Url : FString();
			};
			TFunction<TArray<const APlayerController*>()> Controllers = [this]()
			{
				return Live;
			};
			FPlayServRoomsTestAccess::SetConnections(Server, MoveTemp(UrlOf), MoveTemp(Controllers));
		}

		/** A connection logged in with this travel URL, or a seamless swap handed it to this controller. */
		void Open(const APlayerController* Player, const FString& Url)
		{
			Urls.Add(Player, Url);
			Live.AddUnique(Player);
		}

		/** The controller left its world. A closing connection is taken off it before the logout (APlayerController::OnNetCleanup). */
		void Close(const APlayerController* Player)
		{
			Urls.Remove(Player);
			Live.Remove(Player);
		}
	};

	/** Deliver a ping and run the module's maintenance at the clock's time — one tick of a server that is otherwise idle. */
	void TickAt(UPlayServRooms* Server, FPlayServFakeUplinkTransport& Socket, double Now)
	{
		Socket.SimulateMessage(TEXT("{\"type\":\"ping\"}"));
		FPlayServRoomsTestAccess::Tick(Server, Now);
	}

	/** A hosting server on a scripted socket, Ready with pushed-ticket admission, holding one registered room with a 30 s reconnect grace. */
	TSharedPtr<FPlayServFakeUplinkTransport> BeginHostingOneRoom(UPlayServRooms* Server, FPlayServFakeUplinkFactory& Factory, FFakeClock& Clock, const FString& RoomName)
	{
		FPlayServRoomsTestAccess::BeginWithUplink(Server, Factory.Make(), Clock.Fn(), TEXT("blob-arena"));
		TSharedPtr<FPlayServFakeUplinkTransport> Socket = Factory.Current();
		Socket->SimulateConnected();
		Socket->SimulateMessage(MakeHelloAck(TEXT("push"), DefaultRoomConfigJson()));
		FPlayServRoomsTestAccess::AddRegisteredRoom(Server, MakeSnapshot(RoomName), Clock.Now);
		Server->SetReconnectGraceSeconds(30.0f);
		return Socket;
	}

	/** The platform pushes a ticket, PreLogin verifies it, and the engine logs the connection in: a member, admitted the normal way. */
	APlayerController* AdmitMember(UPlayServRooms* Server, FPlayServFakeUplinkTransport& Socket, FScriptedConnections& Connections, const FString& RoomName, const FString& PlayerId, const FString& Token)
	{
		Socket.SimulateMessage(FString::Printf(TEXT("{\"type\":\"ticket_offer\",\"reservation_token\":\"%s\",\"room_name\":\"%s\",\"player_id\":\"%s\",\"expires_in\":10}"), *Token, *RoomName, *PlayerId));
		Server->VerifyTicket(Token);
		APlayerController* Player = NewObject<APlayerController>(GetTransientPackage());
		const FString Url = FString::Printf(TEXT("203.0.113.5:7777?rsv=%s"), *Token);
		Connections.Open(Player, Url);
		FPlayServRoomsTestAccess::PostLogin(Server, Player, Url);
		return Player;
	}

	/** Drive a client on a fake factory to Ready with the given ack. Returns the current fake socket. */
	TSharedPtr<FPlayServFakeUplinkTransport> BringClientToReady(FPlayServUplinkClient& Client, FPlayServFakeUplinkFactory& Factory, const FString& Ack)
	{
		FPlayServUplinkHelloParams Hello;
		Hello.ExecutorSlug = TEXT("blob-arena");
		Hello.InstanceId = TEXT("inst-1");
		Hello.Capabilities.Add(TEXT("admission_push"));
		Client.Start(TEXT("wss://dev.example/uplink"), TEXT("sk_test"), Hello);
		TSharedPtr<FPlayServFakeUplinkTransport> Socket = Factory.Current();
		Socket->SimulateConnected();
		Socket->SimulateMessage(Ack);
		return Socket;
	}
}

// ---------------------------------------------------------------------------
// PlayServ.Rooms.RosterHash.MatchesContractVectors
//
// game-rooms/ADR-005 §2 fixes the canonical roster hash and gives test vectors; the platform's
// RosterHashTests.cs adds one more. A hash that differs by one byte makes every heartbeat a
// mismatch, so the vectors are pinned here byte for byte.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServRoomsRosterHashVectorsTest,
	"PlayServ.Rooms.RosterHash.MatchesContractVectors",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServRoomsRosterHashVectorsTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("SHA-256 of 'abc' (FIPS 180-4 vector)"),
		PlayServRosterHash::Sha256Hex(FString(TEXT("abc"))),
		FString(TEXT("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")));
	TestEqual(TEXT("SHA-256 of 56 bytes crosses the padding boundary"),
		PlayServRosterHash::Sha256Hex(FString(TEXT("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))),
		FString(TEXT("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1")));

	TestEqual(TEXT("empty roster hashes the empty string"),
		PlayServRosterHash::Compute({}),
		FString(TEXT("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")));
	TestEqual(TEXT("one player (ADR vector 2)"),
		PlayServRosterHash::Compute({ TEXT("plr_01J8ZKQ4X0") }),
		FString(TEXT("761b2b3a0187ba8698d89cb10312305c5c1951800b94b6191fb8a613dc5f88d2")));
	TestEqual(TEXT("three players out of order (ADR vector 3)"),
		PlayServRosterHash::Compute({ TEXT("plr_01J8ZKQ4X2"), TEXT("plr_01J8ZKQ4X0"), TEXT("plr_01J8ZKQ4X1") }),
		FString(TEXT("26741b065f492be34e5fe44755321f11313291699161950ee0ba800ef2d2c57e")));
	TestEqual(TEXT("platform RosterHashTests vector (plr_c, plr_a, plr_b)"),
		PlayServRosterHash::Compute({ TEXT("plr_c"), TEXT("plr_a"), TEXT("plr_b") }),
		FString(TEXT("d96c046793ed4ae06cfbe9def7664e79a470399bd51d18988cde05f657522238")));

	TestEqual(TEXT("duplicates collapse"),
		PlayServRosterHash::Compute({ TEXT("plr_a"), TEXT("plr_a") }),
		PlayServRosterHash::Compute({ TEXT("plr_a") }));
	TestEqual(TEXT("ordinal sort puts uppercase before lowercase"),
		PlayServRosterHash::CanonicalRoster({ TEXT("plr_a"), TEXT("plr_B") }),
		FString(TEXT("plr_B\nplr_a")));
	TestEqual(TEXT("roster_count is the distinct count"),
		PlayServRosterHash::Count({ TEXT("plr_a"), TEXT("plr_a"), TEXT("plr_b"), TEXT("") }), 2);
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Rooms.Validation.MirrorsContractRules
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServRoomsValidationTest,
	"PlayServ.Rooms.Validation.MirrorsContractRules",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServRoomsValidationTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("room name: simple"), PlayServRoomsValidation::IsValidRoomName(TEXT("blob-7a3f")));
	TestTrue(TEXT("room name: full alphabet"), PlayServRoomsValidation::IsValidRoomName(TEXT("A:b.c_d-1")));
	TestFalse(TEXT("room name: empty"), PlayServRoomsValidation::IsValidRoomName(TEXT("")));
	TestFalse(TEXT("room name: leading dash"), PlayServRoomsValidation::IsValidRoomName(TEXT("-x")));
	TestFalse(TEXT("room name: space (a display name belongs in attributes)"), PlayServRoomsValidation::IsValidRoomName(TEXT("Blob Arena")));
	TestFalse(TEXT("room name: 65 characters"), PlayServRoomsValidation::IsValidRoomName(FString::ChrN(65, TEXT('a'))));
	TestTrue(TEXT("room name: 64 characters"), PlayServRoomsValidation::IsValidRoomName(FString::ChrN(64, TEXT('a'))));
	TestFalse(TEXT("room name: non-ASCII"), PlayServRoomsValidation::IsValidRoomName(TEXT("ünicode")));

	TestTrue(TEXT("instance id: GUID with hyphens"), PlayServRoomsValidation::IsValidInstanceId(TEXT("d3f1c8a2-0000-4000-8000-000000000001")));
	TestFalse(TEXT("instance id: leading dot"), PlayServRoomsValidation::IsValidInstanceId(TEXT(".x")));

	TestTrue(TEXT("region: 32 characters"), PlayServRoomsValidation::IsValidRegion(FString::ChrN(32, TEXT('r'))));
	TestFalse(TEXT("region: 33 characters"), PlayServRoomsValidation::IsValidRegion(FString::ChrN(33, TEXT('r'))));

	TMap<FString, FString> Small;
	Small.Add(TEXT("map"), TEXT("arena"));
	TestTrue(TEXT("attributes: small object is under the bound"), PlayServRoomsValidation::AttributesByteSize(Small) < PlayServRoomsValidation::MaxAttributesBytes);
	TMap<FString, FString> Large;
	Large.Add(TEXT("blob"), FString::ChrN(3000, TEXT('x')));
	TestTrue(TEXT("attributes: 3000-byte value is over the bound"), PlayServRoomsValidation::AttributesByteSize(Large) > PlayServRoomsValidation::MaxAttributesBytes);

	FPlayServRoomSnapshot Good = MakeSnapshot(TEXT("blob-7a3f"));
	TestFalse(TEXT("a good snapshot validates"), PlayServRoomsValidation::ValidateSnapshot(Good).IsError());

	FPlayServRoomSnapshot BadName = Good;
	BadName.RoomName = TEXT("Blob Arena");
	TestEqual(TEXT("bad room name is ValidationFailed"), PlayServRoomsValidation::ValidateSnapshot(BadName).Code, EPlayServErrorCode::ValidationFailed);

	FPlayServRoomSnapshot BadPort = Good;
	BadPort.Connect.Port = 70000;
	TestTrue(TEXT("port out of range is refused"), PlayServRoomsValidation::ValidateSnapshot(BadPort).IsError());

	FPlayServRoomSnapshot BigAttributes = Good;
	BigAttributes.Attributes = Large;
	TestTrue(TEXT("oversize attributes are refused before the wire"), PlayServRoomsValidation::ValidateSnapshot(BigAttributes).Message.Contains(TEXT("attributes_too_large")));

	EPlayServRoomTransport Transport = EPlayServRoomTransport::Udp;
	TestTrue(TEXT("transport parses case-insensitively"), PlayServRoomsValidation::TransportFromWire(TEXT("TCP"), Transport) && Transport == EPlayServRoomTransport::Tcp);
	TestFalse(TEXT("unknown transport is refused"), PlayServRoomsValidation::TransportFromWire(TEXT("quic"), Transport));
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Rooms.Paths.RoomsNamespaceWithTwins
//
// Decision 27: every rooms call lives under /rooms/{slug}/…; the /matchmaking/{slug}/… twins
// stay served through the migration window and are reachable only by the explicit toggle.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServRoomsPathsTest,
	"PlayServ.Rooms.Paths.RoomsNamespaceWithTwins",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServRoomsPathsTest::RunTest(const FString& Parameters)
{
	const bool bPrevious = PlayServRoomsPaths::UsesMatchmakingTwins();
	PlayServRoomsPaths::SetUseMatchmakingTwins(false);
	TestEqual(TEXT("upsert"), PlayServRoomsPaths::Upsert(TEXT("blob-arena")), FString(TEXT("/rooms/blob-arena:upsert")));
	TestEqual(TEXT("close"), PlayServRoomsPaths::Close(TEXT("blob-arena"), TEXT("blob-7a3f")), FString(TEXT("/rooms/blob-arena/blob-7a3f:close")));
	TestEqual(TEXT("config"), PlayServRoomsPaths::Config(TEXT("blob-arena")), FString(TEXT("/rooms/blob-arena/config")));
	TestEqual(TEXT("list"), PlayServRoomsPaths::List(TEXT("blob-arena")), FString(TEXT("/rooms/blob-arena")));
	TestEqual(TEXT("browse"), PlayServRoomsPaths::Browse(TEXT("blob-arena")), FString(TEXT("/rooms/blob-arena:browse")));
	TestEqual(TEXT("join"), PlayServRoomsPaths::Join(TEXT("blob-arena"), TEXT("blob-7a3f")), FString(TEXT("/rooms/blob-arena/blob-7a3f:join")));
	TestEqual(TEXT("host"), PlayServRoomsPaths::Host(TEXT("blob-arena")), FString(TEXT("/rooms/blob-arena:host")));
	TestEqual(TEXT("uplink url"), PlayServRoomsPaths::UplinkUrl(TEXT("https://dev.platform.playserv.io/")), FString(TEXT("wss://dev.platform.playserv.io/uplink")));

	PlayServRoomsPaths::SetUseMatchmakingTwins(true);
	TestEqual(TEXT("twin upsert"), PlayServRoomsPaths::Upsert(TEXT("blob-arena")), FString(TEXT("/matchmaking/blob-arena/rooms/upsert")));
	TestEqual(TEXT("twin close"), PlayServRoomsPaths::Close(TEXT("blob-arena"), TEXT("blob-7a3f")), FString(TEXT("/matchmaking/blob-arena/rooms/blob-7a3f:close")));
	TestEqual(TEXT("twin config"), PlayServRoomsPaths::Config(TEXT("blob-arena")), FString(TEXT("/matchmaking/blob-arena/room-config")));
	TestEqual(TEXT("twin browse"), PlayServRoomsPaths::Browse(TEXT("blob-arena")), FString(TEXT("/matchmaking/blob-arena/rooms:browse")));
	TestEqual(TEXT("join has no twin"), PlayServRoomsPaths::Join(TEXT("blob-arena"), TEXT("blob-7a3f")), FString(TEXT("/rooms/blob-arena/blob-7a3f:join")));
	TestEqual(TEXT("host has no twin"), PlayServRoomsPaths::Host(TEXT("blob-arena")), FString(TEXT("/rooms/blob-arena:host")));
	PlayServRoomsPaths::SetUseMatchmakingTwins(bPrevious);
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Rooms.Heartbeat.BodyAndAckFollowTheContract
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServRoomsHeartbeatWireTest,
	"PlayServ.Rooms.Heartbeat.BodyAndAckFollowTheContract",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServRoomsHeartbeatWireTest::RunTest(const FString& Parameters)
{
	FPlayServRoomRuntime Room;
	Room.Snapshot = MakeSnapshot(TEXT("blob-7a3f"));
	Room.Snapshot.bOpen = false;
	Room.Roster.Add(TEXT("plr_b"));
	Room.Roster.Add(TEXT("plr_a"));

	const TSharedPtr<FJsonObject> Body = PlayServRoomRuntime::BuildUpsertBody(Room, TEXT("inst-1"));
	TestEqual(TEXT("room_name"), Body->GetStringField(TEXT("room_name")), FString(TEXT("blob-7a3f")));
	TestEqual(TEXT("players is the roster size"), static_cast<int32>(Body->GetNumberField(TEXT("players"))), 2);
	TestEqual(TEXT("capacity"), static_cast<int32>(Body->GetNumberField(TEXT("capacity"))), 16);
	TestEqual(TEXT("state"), Body->GetStringField(TEXT("state")), FString(TEXT("lobby")));
	TestFalse(TEXT("open"), Body->GetBoolField(TEXT("open")));
	TestEqual(TEXT("attributes.map"), Body->GetObjectField(TEXT("attributes"))->GetStringField(TEXT("map")), FString(TEXT("arena")));
	TestEqual(TEXT("connect.host"), Body->GetObjectField(TEXT("connect"))->GetStringField(TEXT("host")), FString(TEXT("203.0.113.5")));
	TestEqual(TEXT("connect.port"), static_cast<int32>(Body->GetObjectField(TEXT("connect"))->GetNumberField(TEXT("port"))), 7777);
	TestEqual(TEXT("connect.transport"), Body->GetObjectField(TEXT("connect"))->GetStringField(TEXT("transport")), FString(TEXT("udp")));
	TestEqual(TEXT("region"), Body->GetStringField(TEXT("region")), FString(TEXT("eu-west-1")));
	TestEqual(TEXT("instance_id"), Body->GetStringField(TEXT("instance_id")), FString(TEXT("inst-1")));
	TestEqual(TEXT("roster_hash is the canonical hash"), Body->GetStringField(TEXT("roster_hash")), PlayServRosterHash::Compute({ TEXT("plr_a"), TEXT("plr_b") }));
	TestEqual(TEXT("roster_count"), static_cast<int32>(Body->GetNumberField(TEXT("roster_count"))), 2);
	TestFalse(TEXT("no roster array on the heartbeat (decision 11)"), Body->HasField(TEXT("roster")));

	const TSharedPtr<FJsonObject> AckJson = FPlayServFakeUplinkTransport::ParseJson(TEXT(
		"{\"created\":true,\"placement\":{\"state\":\"draining\",\"open\":true,\"draining\":true,\"drain_cause\":\"lifetime\",\"drain_until\":null,\"open_refused\":false},"
		"\"roster_check\":\"mismatch\",\"room_config\":{\"function_id\":\"fn_x\",\"function_slug\":\"blob-arena\",\"capacity\":8,\"reservation_ttl_seconds\":10,"
		"\"room_lifetime_seconds\":0,\"room_idle_timeout_seconds\":null,\"max_rooms\":20,\"version\":7,\"updated_at\":\"2026-09-12T00:00:00Z\",\"updated_by\":\"usr_1\"}}"));
	FPlayServUpsertAck Ack;
	TestTrue(TEXT("ack parses"), FPlayServUpsertAck::Parse(AckJson, Ack));
	TestTrue(TEXT("created"), Ack.bCreated);
	TestTrue(TEXT("roster_check mismatch"), Ack.RosterCheck == EPlayServRosterCheck::Mismatch);
	TestTrue(TEXT("placement draining"), Ack.Placement == EPlayServPlacementState::Draining);
	TestTrue(TEXT("room_config present"), Ack.bHasRoomConfig);
	TestEqual(TEXT("room_config version"), Ack.RoomConfig.Version, 7);
	TestFalse(TEXT("null idle timeout means none"), Ack.RoomConfig.bHasIdleTimeout);
	TestEqual(TEXT("room_config max_rooms"), Ack.RoomConfig.MaxRooms, 20);

	const TSharedPtr<FJsonObject> Join = PlayServRoomRuntime::BuildPresenceDelta(TEXT("blob-7a3f"), 1700000000000LL, PlayServRoomsWire::EventJoin, TEXT("plr_a"), TEXT("rsv_1"));
	TestEqual(TEXT("join frame type"), Join->GetStringField(TEXT("type")), FString(TEXT("room_presence")));
	TestEqual(TEXT("join frame event"), Join->GetStringField(TEXT("event")), FString(TEXT("join")));
	TestEqual(TEXT("join frame token"), Join->GetStringField(TEXT("reservation_token")), FString(TEXT("rsv_1")));
	TestTrue(TEXT("join frame connection_id is null"), Join->HasField(TEXT("connection_id")) && Join->GetField<EJson::None>(TEXT("connection_id"))->IsNull());

	const TSharedPtr<FJsonObject> Leave = PlayServRoomRuntime::BuildPresenceDelta(TEXT("blob-7a3f"), 2, PlayServRoomsWire::EventLeave, TEXT("plr_a"), FString());
	TestFalse(TEXT("leave frame carries no token"), Leave->HasField(TEXT("reservation_token")));

	const TSharedPtr<FJsonObject> Roster = PlayServRoomRuntime::BuildRosterFrame(TEXT("blob-7a3f"), 3, { TEXT("plr_a"), TEXT("plr_b") });
	TestEqual(TEXT("roster frame members"), Roster->GetArrayField(TEXT("players")).Num(), 2);

	FPlayServRoomRuntime SeqRoom;
	const int64 First = SeqRoom.NextSeq(5000);
	const int64 Second = SeqRoom.NextSeq(5000);
	TestTrue(TEXT("seq floors at unix ms and stays strictly increasing"), First == 5000 && Second == 5001);
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Rooms.Uplink.HelloAckPingAndReconnect
//
// The dial-in handshake as GameServerUplinkPool.cs speaks it: hello first, ack makes Ready,
// JSON ping answered with pong, 15 s of silence is a dead socket, the same instance id returns.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServRoomsUplinkHandshakeTest,
	"PlayServ.Rooms.Uplink.HelloAckPingAndReconnect",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServRoomsUplinkHandshakeTest::RunTest(const FString& Parameters)
{
	// The oversize-frame refusal below is logged at Error level on purpose (a customer must see it).
	AddExpectedError(TEXT("exceeds the 1 MiB cap"), EAutomationExpectedErrorFlags::Contains, 1);

	FFakeClock Clock;
	FPlayServFakeUplinkFactory Factory;
	TSharedRef<FPlayServUplinkClient> Client = MakeShared<FPlayServUplinkClient>(Factory.Make(), Clock.Fn());

	int32 ReadyCount = 0;
	FPlayServUplinkAck LastAck;
	Client->OnReady.BindLambda([&ReadyCount, &LastAck](const FPlayServUplinkAck& Ack, int32 Generation)
	{
		++ReadyCount;
		LastAck = Ack;
	});

	TSharedPtr<FPlayServFakeUplinkTransport> Socket = BringClientToReady(*Client, Factory, MakeHelloAck(TEXT("push"), DefaultRoomConfigJson()));
	TestEqual(TEXT("factory received the credential for the upgrade header"), Factory.LastCredential, FString(TEXT("sk_test")));
	TestTrue(TEXT("connect was called"), Socket->bConnectCalled);

	const TSharedPtr<FJsonObject> Hello = Socket->SentJson(0);
	TestTrue(TEXT("hello is the first frame"), Hello.IsValid());
	TestEqual(TEXT("hello type"), Hello->GetStringField(TEXT("type")), FString(TEXT("uplink_hello")));
	TestEqual(TEXT("hello executor_slug"), Hello->GetStringField(TEXT("executor_slug")), FString(TEXT("blob-arena")));
	TestEqual(TEXT("hello instance_id"), Hello->GetStringField(TEXT("instance_id")), FString(TEXT("inst-1")));
	TestEqual(TEXT("hello protocol_version"), static_cast<int32>(Hello->GetNumberField(TEXT("protocol_version"))), 1);
	TestEqual(TEXT("hello capabilities"), Hello->GetArrayField(TEXT("capabilities"))[0]->AsString(), FString(TEXT("admission_push")));
	TestFalse(TEXT("hello carries no secret"), Hello->HasField(TEXT("authorization")));

	TestTrue(TEXT("ack makes the client Ready"), Client->GetState() == EPlayServUplinkState::Ready);
	TestEqual(TEXT("OnReady fired once"), ReadyCount, 1);
	TestEqual(TEXT("session token stored"), LastAck.SessionToken, FString(TEXT("eyJ.test.token")));
	TestEqual(TEXT("admission parsed"), LastAck.Admission, FString(TEXT("push")));
	TestTrue(TEXT("room_config parsed"), LastAck.bHasRoomConfig && LastAck.RoomConfig.Capacity == 8 && LastAck.RoomConfig.bHasIdleTimeout && LastAck.RoomConfig.RoomIdleTimeoutSeconds == 120);

	Socket->SimulateMessage(TEXT("{\"type\":\"ping\"}"));
	TestEqual(TEXT("ping is answered with pong"), Socket->LastSentJson()->GetStringField(TEXT("type")), FString(TEXT("pong")));

	Socket->SimulateMessage(TEXT("{\"type\":\"unknown_frame\",\"x\":1}"));
	TestTrue(TEXT("an unknown frame does not drop the socket"), Client->GetState() == EPlayServUplinkState::Ready);

	Clock.Now += 16.0;
	Client->Tick(Clock.Now);
	TestTrue(TEXT("15 s without a ping is a dead socket"), Client->GetState() == EPlayServUplinkState::Disconnected);
	TestFalse(TEXT("the dead socket was closed"), Socket->bConnected);

	Clock.Now += 31.0;
	Client->Tick(Clock.Now);
	TestEqual(TEXT("a reconnect opened a second socket"), Factory.Created.Num(), 2);
	TSharedPtr<FPlayServFakeUplinkTransport> Second = Factory.Current();
	Second->SimulateConnected();
	TestEqual(TEXT("the hello is re-sent with the same instance id"), Second->SentJson(0)->GetStringField(TEXT("instance_id")), FString(TEXT("inst-1")));
	Second->SimulateMessage(MakeHelloAck(TEXT("push"), DefaultRoomConfigJson()));
	TestEqual(TEXT("generation increments on every ack"), Client->GetGeneration(), 2);

	TSharedPtr<FJsonObject> Huge = MakeShared<FJsonObject>();
	Huge->SetStringField(TEXT("type"), TEXT("room_presence"));
	Huge->SetStringField(TEXT("blob"), FString::ChrN(PlayServRoomsWire::MaxFrameBytes + 16, TEXT('x')));
	const int32 SentBefore = Second->Sent.Num();
	TestFalse(TEXT("an oversize frame is refused locally"), Client->SendFrame(Huge));
	TestEqual(TEXT("nothing left the socket"), Second->Sent.Num(), SentBefore);

	Client->Stop();
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Rooms.Uplink.ServiceRestartReconnects
//
// A platform deploy drains the old revision by closing every game-server uplink with 1012 (Service Restart). It is
// not a refusal: the client reconnects at the first backoff step (1 s, +-20%), and the hello ack of the new
// connection resets the backoff, so the next deploy's close reconnects as quickly (a second step would be 1.6-2.4 s).
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServRoomsUplinkServiceRestartReconnectsTest,
	"PlayServ.Rooms.Uplink.ServiceRestartReconnects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServRoomsUplinkServiceRestartReconnectsTest::RunTest(const FString& Parameters)
{
	FFakeClock Clock;
	FPlayServFakeUplinkFactory Factory;
	TSharedRef<FPlayServUplinkClient> Client = MakeShared<FPlayServUplinkClient>(Factory.Make(), Clock.Fn());
	int32 Refusals = 0;
	Client->OnRefused.BindLambda([&Refusals](const FString&, bool) { ++Refusals; });

	TSharedPtr<FPlayServFakeUplinkTransport> First = BringClientToReady(*Client, Factory, MakeHelloAck(TEXT("push"), DefaultRoomConfigJson()));
	First->SimulateClosed(1012, TEXT("Service Restart"));
	TestFalse(TEXT("a 1012 close does not stop the client"), Client->IsStopped());
	TestEqual(TEXT("a 1012 close is not a refusal"), Refusals, 0);

	Clock.Now += 1.25;
	Client->Tick(Clock.Now);
	TestEqual(TEXT("the client reconnects at the first backoff step"), Factory.Created.Num(), 2);

	TSharedPtr<FPlayServFakeUplinkTransport> Second = Factory.Current();
	Second->SimulateConnected();
	Second->SimulateMessage(MakeHelloAck(TEXT("push"), DefaultRoomConfigJson()));
	Second->SimulateClosed(1012, TEXT("Service Restart"));
	Clock.Now += 1.25;
	Client->Tick(Clock.Now);
	TestEqual(TEXT("the next deploy's close reconnects as quickly: the hello ack reset the backoff"), Factory.Created.Num(), 3);
	TestFalse(TEXT("still not stopped"), Client->IsStopped());
	TestEqual(TEXT("still no refusal"), Refusals, 0);

	Client->Stop();
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Rooms.Uplink.RefusalsAndTokenRenewal
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServRoomsUplinkRefusalTest,
	"PlayServ.Rooms.Uplink.RefusalsAndTokenRenewal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServRoomsUplinkRefusalTest::RunTest(const FString& Parameters)
{
	// A permanent platform refusal is logged at Error level on purpose (a customer must see it).
	AddExpectedError(TEXT("refused this process"), EAutomationExpectedErrorFlags::Contains, 1);

	FFakeClock Clock;
	FPlayServFakeUplinkFactory Factory;
	TSharedRef<FPlayServUplinkClient> Client = MakeShared<FPlayServUplinkClient>(Factory.Make(), Clock.Fn());
	FString RefusedReason;
	bool bRefusedPermanent = false;
	Client->OnRefused.BindLambda([&RefusedReason, &bRefusedPermanent](const FString& Reason, bool bPermanent)
	{
		RefusedReason = Reason;
		bRefusedPermanent = bPermanent;
	});

	FPlayServUplinkHelloParams Hello;
	Hello.ExecutorSlug = TEXT("blob-arena");
	Hello.InstanceId = TEXT("inst-1");
	Client->Start(TEXT("wss://dev.example/uplink"), TEXT("sk_test"), Hello);
	Factory.Current()->SimulateConnected();
	Factory.Current()->SimulateClosed(1008, TEXT("instance_id_conflict"));
	TestEqual(TEXT("conflict is reported"), RefusedReason, FString(TEXT("instance_id_conflict")));
	TestFalse(TEXT("conflict is not permanent"), bRefusedPermanent);
	Clock.Now += 5.0;
	Client->Tick(Clock.Now);
	TestEqual(TEXT("conflict waits out the platform's 10 s window"), Factory.Created.Num(), 1);
	Clock.Now += 6.0;
	Client->Tick(Clock.Now);
	TestEqual(TEXT("reconnects after the window"), Factory.Created.Num(), 2);

	Factory.Current()->SimulateConnected();
	Factory.Current()->SimulateClosed(1008, TEXT("executor_not_found"));
	TestTrue(TEXT("unknown executor is permanent"), bRefusedPermanent && Client->IsStopped());
	Clock.Now += 120.0;
	Client->Tick(Clock.Now);
	TestEqual(TEXT("no reconnect after a permanent refusal"), Factory.Created.Num(), 2);

	TestTrue(TEXT("permanent set"), FPlayServUplinkClient::IsPermanentRefusal(TEXT("protocol_unsupported")) && FPlayServUplinkClient::IsPermanentRefusal(TEXT("instance_id_missing")));
	TestFalse(TEXT("handshake_timeout is transient"), FPlayServUplinkClient::IsPermanentRefusal(TEXT("handshake_timeout")));

	// Token renewal: the platform never sends the renewal frame, so the client re-hellos before expiry.
	FFakeClock Clock2;
	FPlayServFakeUplinkFactory Factory2;
	TSharedRef<FPlayServUplinkClient> Client2 = MakeShared<FPlayServUplinkClient>(Factory2.Make(), Clock2.Fn());
	TSharedPtr<FPlayServFakeUplinkTransport> Socket = BringClientToReady(*Client2, Factory2, MakeHelloAck(TEXT("push"), DefaultRoomConfigJson(), 3600));
	const double Margin = FPlayServUplinkClient::RenewalMarginSeconds(3600);
	TestEqual(TEXT("renewal margin is a tenth of the lifetime"), Margin, 360.0);
	const double AckedAt = Clock2.Now;
	while (Clock2.Now < AckedAt + 3600.0 - Margin - 5.0)
	{
		Clock2.Now += 5.0;
		Socket->SimulateMessage(TEXT("{\"type\":\"ping\"}"));
		Client2->Tick(Clock2.Now);
	}
	TestTrue(TEXT("still Ready before the margin"), Client2->GetState() == EPlayServUplinkState::Ready);
	Clock2.Now = AckedAt + 3600.0 - Margin + 1.0;
	Socket->SimulateMessage(TEXT("{\"type\":\"ping\"}"));
	Client2->Tick(Clock2.Now);
	TestTrue(TEXT("re-hello is scheduled inside the margin"), Client2->GetState() != EPlayServUplinkState::Ready);
	Clock2.Now += 1.0;
	Client2->Tick(Clock2.Now);
	TestEqual(TEXT("a fresh socket carries the re-hello"), Factory2.Created.Num(), 2);

	TestTrue(TEXT("backoff doubles and caps"), FPlayServUplinkClient::ReconnectDelaySeconds(0, 0.5f) == 1.0
		&& FPlayServUplinkClient::ReconnectDelaySeconds(3, 0.5f) == 8.0
		&& FPlayServUplinkClient::ReconnectDelaySeconds(9, 0.5f) == 30.0);
	Client->Stop();
	Client2->Stop();
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Rooms.Uplink.ShutdownClosesTheSocketOnce
//
// The live transport over a scripted engine socket. The engine's socket reports connected until
// the closing handshake completes, so after the uplink client's shutdown asked it to close, the
// transport's destructor asked again — `LogWebSockets: Warning: FLwsWebSocket[1]::Close: Already
// closing` at every hosted server's exit, in the 40 lines of output the platform keeps (BlobEater
// hosting-test register #25, PSV-2811). A socket nobody closed is still closed once when its
// transport goes.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServRoomsUplinkClosesOnceTest,
	"PlayServ.Rooms.Uplink.ShutdownClosesTheSocketOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServRoomsUplinkClosesOnceTest::RunTest(const FString& Parameters)
{
	FFakeClock Clock;
	TArray<TSharedPtr<FPlayServScriptedWebSocket>> Sockets;
	FPlayServUplinkTransportFactory Factory = [&Sockets](const FString& Url, const FString& Credential)
	{
		TSharedPtr<FPlayServScriptedWebSocket> Socket = MakeShared<FPlayServScriptedWebSocket>();
		Sockets.Add(Socket);
		return FPlayServUplinkTransportTestAccess::MakeLiveTransport(Url, Credential, Socket);
	};
	TSharedRef<FPlayServUplinkClient> Client = MakeShared<FPlayServUplinkClient>(Factory, Clock.Fn());
	FPlayServUplinkHelloParams Hello;
	Hello.ExecutorSlug = TEXT("blob-arena");
	Hello.InstanceId = TEXT("inst-1");
	Client->Start(TEXT("wss://dev.example/uplink"), TEXT("sk_test"), Hello);
	if (!TestEqual(TEXT("the client opened one live transport"), Sockets.Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("its engine socket is open"), Sockets[0]->IsConnected());

	Client->Stop();
	TestEqual(TEXT("the shutdown closed the engine socket once, and the transport it dropped did not close it again"), Sockets[0]->CloseCalls, 1);

	TSharedPtr<FPlayServScriptedWebSocket> Unclosed = MakeShared<FPlayServScriptedWebSocket>();
	TSharedPtr<FPlayServUplinkTransport> Transport = FPlayServUplinkTransportTestAccess::MakeLiveTransport(TEXT("wss://dev.example/uplink"), TEXT("sk_test"), Unclosed);
	Transport->Connect();
	Transport.Reset();
	TestEqual(TEXT("a transport dropped with its socket open closes it once"), Unclosed->CloseCalls, 1);
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Rooms.Admission.TableRedeemsOnceAndTypesRefusals
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServRoomsAdmissionTableTest,
	"PlayServ.Rooms.Admission.TableRedeemsOnceAndTypesRefusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServRoomsAdmissionTableTest::RunTest(const FString& Parameters)
{
	FPlayServAdmissionTable Table;
	FPlayServPushedTicket Ticket;
	Ticket.ReservationToken = TEXT("rsv_1");
	Ticket.RoomName = TEXT("blob-7a3f");
	Ticket.PlayerId = TEXT("plr_a");
	Ticket.ExpiresAt = 110.0;
	Table.Add(Ticket);

	FPlayServPushedTicket Redeemed;
	FString Reason;
	TestFalse(TEXT("unknown token"), Table.TryRedeem(TEXT("rsv_nope"), 100.0, FString(), Redeemed, Reason));
	TestEqual(TEXT("unknown is reservation_invalid"), Reason, FString(TEXT("reservation_invalid")));
	TestFalse(TEXT("wrong room"), Table.TryRedeem(TEXT("rsv_1"), 100.0, TEXT("other-room"), Redeemed, Reason));
	TestEqual(TEXT("wrong room is room_mismatch"), Reason, FString(TEXT("room_mismatch")));
	TestTrue(TEXT("valid ticket redeems"), Table.TryRedeem(TEXT("rsv_1"), 100.0, FString(), Redeemed, Reason));
	TestEqual(TEXT("player id comes with it"), Redeemed.PlayerId, FString(TEXT("plr_a")));
	TestFalse(TEXT("replay"), Table.TryRedeem(TEXT("rsv_1"), 101.0, FString(), Redeemed, Reason));
	TestEqual(TEXT("replay is reservation_consumed"), Reason, FString(TEXT("reservation_consumed")));

	FPlayServPushedTicket Late = Ticket;
	Late.ReservationToken = TEXT("rsv_2");
	Table.Add(Late);
	TestFalse(TEXT("expired ticket"), Table.TryRedeem(TEXT("rsv_2"), 110.0, FString(), Redeemed, Reason));
	TestEqual(TEXT("expired is reservation_expired"), Reason, FString(TEXT("reservation_expired")));

	TArray<FPlayServPushedTicket> Expired;
	Table.SweepExpired(111.0, Expired);
	TestEqual(TEXT("sweep returns the unredeemed expired ticket only"), Expired.Num(), 1);
	TestEqual(TEXT("the redeemed one is remembered for replays"), Table.Num(), 1);

	TestEqual(TEXT("travel option after the map"), FPlayServAdmissionTable::ParseTravelOption(TEXT("/Game/Maps/Arena?rsv=rsv_9?Name=Bob"), TEXT("rsv")), FString(TEXT("rsv_9")));
	TestEqual(TEXT("travel option is case-insensitive"), FPlayServAdmissionTable::ParseTravelOption(TEXT("?RSV=rsv_9"), TEXT("rsv")), FString(TEXT("rsv_9")));
	TestTrue(TEXT("missing option is empty"), FPlayServAdmissionTable::ParseTravelOption(TEXT("?Name=Bob"), TEXT("rsv")).IsEmpty());
	TestTrue(TEXT("web grammar would hide the option — the engine splits on ? only"), FPlayServAdmissionTable::ParseTravelOption(TEXT("?playerId=plr_a&rsv=rsv_9"), TEXT("rsv")).IsEmpty());
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Rooms.Module.PushedTicketsAndRoomLifecycle
//
// The hosting half end to end on a scripted socket: hello with admission_push, a ticket_offer
// answered ok, a veto, a refused join_ack removing the player, a roster repair on a heartbeat
// mismatch, a ticket_release, and a lost-ownership answer ending the room. No HTTP leaves the
// process. Admission and presence have tests of their own below.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServRoomsModuleTest,
	"PlayServ.Rooms.Module.PushedTicketsAndRoomLifecycle",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServRoomsModuleTest::RunTest(const FString& Parameters)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!TestNotNull(TEXT("subsystem"), PS))
	{
		return false;
	}
	UPlayServRooms* Server = PS->GetRooms();
	if (!TestNotNull(TEXT("rooms module"), Server))
	{
		return false;
	}

	FFakeClock Clock;
	FPlayServFakeUplinkFactory Factory;
	FPlayServRoomsTestAccess::BeginWithUplink(Server, Factory.Make(), Clock.Fn(), TEXT("blob-arena"));
	TestTrue(TEXT("module is hosting"), Server->IsHosting());
	TestEqual(TEXT("uplink url derives from BaseURL"), Factory.LastUrl.Right(7), FString(TEXT("/uplink")));

	TSharedPtr<FPlayServFakeUplinkTransport> Socket = Factory.Current();
	Socket->SimulateConnected();
	TestEqual(TEXT("hello executor is the configured slug"), Socket->SentJson(0)->GetStringField(TEXT("executor_slug")), FString(TEXT("blob-arena")));
	TestEqual(TEXT("hello declares admission_push"), Socket->SentJson(0)->GetArrayField(TEXT("capabilities"))[0]->AsString(), FString(TEXT("admission_push")));

	Socket->SimulateMessage(MakeHelloAck(TEXT("push"), DefaultRoomConfigJson()));
	TestTrue(TEXT("uplink Ready"), Server->GetUplinkState() == EPlayServUplinkState::Ready);
	TestTrue(TEXT("room config applied"), Server->HasRoomConfig() && Server->GetRoomConfig().Version == 4);
	TestTrue(TEXT("admission mode is push"), Server->GetAdmissionMode() == EPlayServAdmissionMode::Push);

	const FPlayServRoomSnapshot Snapshot = MakeSnapshot(TEXT("blob-7a3f"));
	FPlayServRoomsTestAccess::AddRegisteredRoom(Server, Snapshot, Clock.Now);

	// An offer for an unknown room is refused room_closed; for a known room it is accepted and stored.
	Socket->SimulateMessage(TEXT("{\"type\":\"ticket_offer\",\"reservation_token\":\"rsv_x\",\"room_name\":\"nope\",\"player_id\":\"plr_z\",\"expires_in\":10}"));
	TSharedPtr<FJsonObject> Refused = Socket->LastSentOfType(TEXT("ticket_result"));
	TestTrue(TEXT("offer for an unknown room refused"), Refused.IsValid() && !Refused->GetBoolField(TEXT("ok")) && Refused->GetStringField(TEXT("reason")) == TEXT("room_closed"));

	Socket->SimulateMessage(TEXT("{\"type\":\"ticket_offer\",\"reservation_token\":\"rsv_1\",\"room_name\":\"blob-7a3f\",\"player_id\":\"plr_a\",\"expires_in\":10,\"params\":{\"team\":\"red\"}}"));
	TSharedPtr<FJsonObject> Accepted = Socket->LastSentOfType(TEXT("ticket_result"));
	TestTrue(TEXT("offer accepted"), Accepted.IsValid() && Accepted->GetBoolField(TEXT("ok")) && Accepted->GetStringField(TEXT("reservation_token")) == TEXT("rsv_1"));
	TestEqual(TEXT("ticket stored"), FPlayServRoomsTestAccess::TicketCount(Server), 1);

	// A veto hook refuses with the room's own detail.
	Server->OnTicketOffer.BindLambda([](const FString& RoomName, const FString& PlayerId, FString& OutDetail)
	{
		OutDetail = TEXT("banned on this server");
		return PlayerId != TEXT("plr_bad");
	});
	Socket->SimulateMessage(TEXT("{\"type\":\"ticket_offer\",\"reservation_token\":\"rsv_bad\",\"room_name\":\"blob-7a3f\",\"player_id\":\"plr_bad\",\"expires_in\":10}"));
	TSharedPtr<FJsonObject> Vetoed = Socket->LastSentOfType(TEXT("ticket_result"));
	TestTrue(TEXT("veto is room_refused with detail"), Vetoed.IsValid() && !Vetoed->GetBoolField(TEXT("ok"))
		&& Vetoed->GetStringField(TEXT("reason")) == TEXT("room_refused") && Vetoed->GetStringField(TEXT("detail")) == TEXT("banned on this server"));
	Server->OnTicketOffer.Unbind();

	// A verified ticket followed by the engine's login puts the player on the roster.
	APlayerController* PlayerA = NewObject<APlayerController>(GetTransientPackage());
	const FPlayServTicketVerdict Verdict = Server->VerifyTicket(TEXT("rsv_1"));
	TestTrue(TEXT("ticket accepted"), Verdict.bAccepted && Verdict.PlayerId == TEXT("plr_a") && Verdict.RoomName == TEXT("blob-7a3f"));
	FPlayServRoomsTestAccess::PostLogin(Server, PlayerA, TEXT("203.0.113.5:7777?rsv=rsv_1"));
	TSharedPtr<FJsonObject> Join = Socket->LastSentOfType(TEXT("room_presence"));
	TestTrue(TEXT("join frame carries the token"), Join.IsValid() && Join->GetStringField(TEXT("event")) == TEXT("join") && Join->GetStringField(TEXT("reservation_token")) == TEXT("rsv_1"));
	TestTrue(TEXT("seq is floored at unix ms"), Join->GetNumberField(TEXT("seq")) > 1.6e12);
	TestEqual(TEXT("player count"), Server->GetRoomPlayerCount(TEXT("blob-7a3f")), 1);
	TestEqual(TEXT("the connection maps to its player id"), Server->GetPlayerId(PlayerA), FString(TEXT("plr_a")));

	// A refused join_ack removes the player.
	Socket->SimulateMessage(TEXT("{\"type\":\"join_ack\",\"room_name\":\"blob-7a3f\",\"player_id\":\"plr_a\",\"reservation_token\":\"rsv_1\",\"ok\":false,\"reason\":\"room_closed\"}"));
	TestFalse(TEXT("refused join removed the player"), FPlayServRoomsTestAccess::RosterContains(Server, TEXT("blob-7a3f"), TEXT("plr_a")));

	// A heartbeat mismatch triggers the roster repair frame.
	Socket->SimulateMessage(TEXT("{\"type\":\"ticket_offer\",\"reservation_token\":\"rsv_c\",\"room_name\":\"blob-7a3f\",\"player_id\":\"plr_c\",\"expires_in\":10}"));
	APlayerController* PlayerC = NewObject<APlayerController>(GetTransientPackage());
	Server->VerifyTicket(TEXT("rsv_c"));
	FPlayServRoomsTestAccess::PostLogin(Server, PlayerC, TEXT("203.0.113.5:7777?rsv=rsv_c"));
	FPlayServRoomsTestAccess::DeliverHeartbeatAnswer(Server, TEXT("blob-7a3f"), 200, FString(),
		TEXT("{\"created\":false,\"placement\":{\"state\":\"open\"},\"roster_check\":\"mismatch\",\"room_config\":null}"));
	TSharedPtr<FJsonObject> Repair = Socket->LastSentOfType(TEXT("room_presence"));
	TestTrue(TEXT("mismatch sends the roster frame"), Repair.IsValid() && Repair->GetStringField(TEXT("event")) == TEXT("roster") && Repair->GetArrayField(TEXT("players")).Num() == 1);
	TestTrue(TEXT("placement applied from the ack"), Server->GetRoomPlacement(TEXT("blob-7a3f")) == EPlayServPlacementState::Open);

	// RemovePlayer is the game's own decision: no grace, a leave frame now.
	const int32 FramesBeforeRemoval = Socket->CountSentOfType(TEXT("room_presence"));
	TestTrue(TEXT("the game removes a member"), Server->RemovePlayer(TEXT("blob-7a3f"), TEXT("plr_c")));
	TSharedPtr<FJsonObject> Leave = Socket->LastSentOfType(TEXT("room_presence"));
	TestTrue(TEXT("leave frame at once"), Socket->CountSentOfType(TEXT("room_presence")) == FramesBeforeRemoval + 1 && Leave->GetStringField(TEXT("event")) == TEXT("leave"));
	TestFalse(TEXT("a removed player is no longer looked up by connection"), Server->GetPlayerId(PlayerC) == TEXT("plr_c"));
	TestFalse(TEXT("removing a non-member is false"), Server->RemovePlayer(TEXT("blob-7a3f"), TEXT("plr_nobody")));

	// A ticket the room gives back, bounded detail.
	Socket->SimulateMessage(TEXT("{\"type\":\"ticket_offer\",\"reservation_token\":\"rsv_d\",\"room_name\":\"blob-7a3f\",\"player_id\":\"plr_d\",\"expires_in\":10}"));
	TestTrue(TEXT("release sends the frame"), FPlayServRoomsTestAccess::ReleaseTicket(Server, TEXT("rsv_d"), TEXT("room_refused"), FString::ChrN(200, TEXT('d'))));
	TSharedPtr<FJsonObject> Release = Socket->LastSentOfType(TEXT("ticket_release"));
	TestTrue(TEXT("release frame shape, detail bounded to 128"), Release.IsValid() && Release->GetStringField(TEXT("reason")) == TEXT("room_refused") && Release->GetStringField(TEXT("detail")).Len() == 128);

	// A lost-ownership heartbeat answer ends the room without a :close.
	FPlayServRoomsTestAccess::DeliverHeartbeatAnswer(Server, TEXT("blob-7a3f"), 409, TEXT("room_owned_by_other_instance"), FString());
	TestEqual(TEXT("the room is gone locally"), Server->GetRoomNames().Num(), 0);

	FPlayServRoomsTestAccess::End(Server);
	TestFalse(TEXT("stopped"), Server->IsHosting());
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Rooms.Ticket.VerifyRedeemsOnceAndTypesRefusals
//
// The one call a hosting game makes: TicketFromOptions reads the engine's option grammar, and
// VerifyTicket redeems the token once against the pushed table. Every refusal is typed, so a
// PreLogin can say why. `admission_unavailable` is what a server with no pushed admission
// answers — unless the development fail-open flag is on, which no shipped build can set.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServRoomsTicketVerifyTest,
	"PlayServ.Rooms.Ticket.VerifyRedeemsOnceAndTypesRefusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServRoomsTicketVerifyTest::RunTest(const FString& Parameters)
{
	// The engine's `?`-delimited grammar, on the whole travel URL and on the options alone.
	TestEqual(TEXT("ticket after the map"), UPlayServRooms::TicketFromOptions(TEXT("/Game/Maps/Arena?rsv=rsv_9?Name=Bob")), FString(TEXT("rsv_9")));
	TestEqual(TEXT("ticket in a host:port URL"), UPlayServRooms::TicketFromOptions(TEXT("203.0.113.5:7777?rsv=rsv_9")), FString(TEXT("rsv_9")));
	TestEqual(TEXT("the key is case-insensitive"), UPlayServRooms::TicketFromOptions(TEXT("?RSV=rsv_9")), FString(TEXT("rsv_9")));
	TestTrue(TEXT("no rsv option is empty"), UPlayServRooms::TicketFromOptions(TEXT("?Name=Bob")).IsEmpty());
	TestTrue(TEXT("the web's &-joined grammar does NOT parse — the engine splits on ? only"),
		UPlayServRooms::TicketFromOptions(TEXT("?playerId=plr_a&rsv=rsv_9")).IsEmpty());

	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!TestNotNull(TEXT("subsystem"), PS))
	{
		return false;
	}
	UPlayServRooms* Server = PS->GetRooms();

	FFakeClock Clock;
	FPlayServFakeUplinkFactory Factory;
	FPlayServRoomsTestAccess::BeginWithUplink(Server, Factory.Make(), Clock.Fn(), TEXT("blob-arena"));
	TSharedPtr<FPlayServFakeUplinkTransport> Socket = Factory.Current();
	Socket->SimulateConnected();

	// Before the ack there is no admission path at all.
	const FPlayServTicketVerdict Early = Server->VerifyTicket(TEXT("rsv_1"));
	TestFalse(TEXT("nothing is verifiable before the hello ack"), Early.bAccepted);
	TestEqual(TEXT("early refusal is admission_unavailable"), Early.Reason, FString(TEXT("admission_unavailable")));

	Socket->SimulateMessage(MakeHelloAck(TEXT("push"), DefaultRoomConfigJson()));
	FPlayServRoomsTestAccess::AddRegisteredRoom(Server, MakeSnapshot(TEXT("blob-7a3f")), Clock.Now);

	Socket->SimulateMessage(TEXT("{\"type\":\"ticket_offer\",\"reservation_token\":\"rsv_1\",\"room_name\":\"blob-7a3f\",\"player_id\":\"plr_a\",\"expires_in\":10}"));
	Socket->SimulateMessage(TEXT("{\"type\":\"ticket_offer\",\"reservation_token\":\"rsv_short\",\"room_name\":\"blob-7a3f\",\"player_id\":\"plr_s\",\"expires_in\":5}"));

	const FPlayServTicketVerdict Unknown = Server->VerifyTicket(TEXT("rsv_nope"));
	TestTrue(TEXT("an unknown token is reservation_invalid, with a message for PreLogin"),
		!Unknown.bAccepted && Unknown.Reason == TEXT("reservation_invalid") && !Unknown.ErrorMessage.IsEmpty());

	const FPlayServTicketVerdict Missing = Server->VerifyTicket(FString());
	TestTrue(TEXT("no ticket at all is reservation_invalid"), !Missing.bAccepted && Missing.Reason == TEXT("reservation_invalid"));

	const FPlayServTicketVerdict Good = Server->VerifyTicket(TEXT("rsv_1"));
	TestTrue(TEXT("a pushed ticket is accepted with its player and room"),
		Good.bAccepted && Good.PlayerId == TEXT("plr_a") && Good.RoomName == TEXT("blob-7a3f") && Good.ReservationToken == TEXT("rsv_1"));
	TestEqual(TEXT("the verdict waits for the login it belongs to"), FPlayServRoomsTestAccess::VerifiedCount(Server), 1);

	const FPlayServTicketVerdict Replay = Server->VerifyTicket(TEXT("rsv_1"));
	TestTrue(TEXT("a replay is reservation_consumed"), !Replay.bAccepted && Replay.Reason == TEXT("reservation_consumed"));

	Clock.Now += 6.0;
	const FPlayServTicketVerdict Expired = Server->VerifyTicket(TEXT("rsv_short"));
	TestTrue(TEXT("a ticket past its TTL is reservation_expired"), !Expired.bAccepted && Expired.Reason == TEXT("reservation_expired"));

	// A ticket whose room this server no longer holds.
	Socket->SimulateMessage(TEXT("{\"type\":\"ticket_offer\",\"reservation_token\":\"rsv_gone\",\"room_name\":\"blob-7a3f\",\"player_id\":\"plr_g\",\"expires_in\":30}"));
	FPlayServRoomsTestAccess::DeliverHeartbeatAnswer(Server, TEXT("blob-7a3f"), 409, TEXT("room_owned_by_other_instance"), FString());
	const FPlayServTicketVerdict Gone = Server->VerifyTicket(TEXT("rsv_gone"));
	TestTrue(TEXT("a ticket for a room this server lost is room_closed"), !Gone.bAccepted && Gone.Reason == TEXT("room_closed"));

	// `consume` is a path this plugin does not implement: Strict refuses, the dev flag admits.
	Socket->SimulateClosed(1001, TEXT("test"));
	Clock.Now += 2.0;
	FPlayServRoomsTestAccess::Tick(Server, Clock.Now);
	TSharedPtr<FPlayServFakeUplinkTransport> Second = Factory.Current();
	Second->SimulateConnected();
	Second->SimulateMessage(MakeHelloAck(TEXT("consume"), DefaultRoomConfigJson()));
	TestTrue(TEXT("admission mode is None on consume"), Server->GetAdmissionMode() == EPlayServAdmissionMode::None);

	const FPlayServTicketVerdict Strict = Server->VerifyTicket(TEXT("rsv_1"));
	TestTrue(TEXT("with no push path the verdict is admission_unavailable"), !Strict.bAccepted && Strict.Reason == TEXT("admission_unavailable"));

	// Development fail-open lets the connection into the GAME, but a player id comes from the
	// platform or not at all — the SDK never invents one, so nothing about this connection is
	// reported and the room's roster stays honest about who the platform actually placed.
	FPlayServRoomsTestAccess::SetAdmissionFailOpen(Server, true);
	const int32 VerdictsBefore = FPlayServRoomsTestAccess::VerifiedCount(Server);
	const FPlayServTicketVerdict Open = Server->VerifyTicket(FString());
	TestTrue(TEXT("fail-open accepts the connection"), Open.bAccepted);
	TestTrue(TEXT("with NO player id — the SDK never invents one"), Open.PlayerId.IsEmpty());
	TestTrue(TEXT("and no token, so nothing can be redeemed"), Open.ReservationToken.IsEmpty());
	TestEqual(TEXT("and it remembers nothing — there is no login it could ever be matched to"),
		FPlayServRoomsTestAccess::VerifiedCount(Server), VerdictsBefore);

	FPlayServRoomsTestAccess::End(Server);
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Rooms.Presence.PostLoginJoinAndLogoutGrace
//
// Presence is the SDK's, not the game's: the engine's own PostLogin and Logout drive it. A
// login joins the room with the token that redeems it; a logout parks the seat for the
// reconnect grace and only reports the leave when the grace lapses; a returning player's
// login cancels the park. A connection with no ticket is reported to nobody.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServRoomsPresenceTest,
	"PlayServ.Rooms.Presence.PostLoginJoinAndLogoutGrace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServRoomsPresenceTest::RunTest(const FString& Parameters)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!TestNotNull(TEXT("subsystem"), PS))
	{
		return false;
	}
	UPlayServRooms* Server = PS->GetRooms();

	// The two loud refusals the module logs when a login reaches it with no verified ticket:
	// both are Error-level on purpose, because the platform refuses such a join with
	// join_ack ok=false reason=reservation_invalid and the player never reaches the roster.
	AddExpectedError(TEXT("logged in with no \\?rsv= ticket"), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedError(TEXT("ticket this server never verified"), EAutomationExpectedErrorFlags::Contains, 1);

	FFakeClock Clock;
	FPlayServFakeUplinkFactory Factory;
	FPlayServRoomsTestAccess::BeginWithUplink(Server, Factory.Make(), Clock.Fn(), TEXT("blob-arena"));
	TSharedPtr<FPlayServFakeUplinkTransport> Socket = Factory.Current();
	Socket->SimulateConnected();
	Socket->SimulateMessage(MakeHelloAck(TEXT("push"), DefaultRoomConfigJson()));
	FPlayServRoomsTestAccess::AddRegisteredRoom(Server, MakeSnapshot(TEXT("blob-7a3f")), Clock.Now);
	Server->SetReconnectGraceSeconds(30.0f);

	APlayerController* Player = NewObject<APlayerController>(GetTransientPackage());
	APlayerController* Bot = NewObject<APlayerController>(GetTransientPackage());
	APlayerController* Stranger = NewObject<APlayerController>(GetTransientPackage());

	// A bot never logs in over a connection, so it never reaches the roster.
	FPlayServRoomsTestAccess::PostLogin(Server, Bot, FString());
	TestEqual(TEXT("a connectionless login reports nothing"), Server->GetRoomPlayerCount(TEXT("blob-7a3f")), 0);

	// A connection with no ticket, and one with a ticket PreLogin never verified.
	FPlayServRoomsTestAccess::PostLogin(Server, Stranger, TEXT("203.0.113.5:7777?Name=Bob"));
	FPlayServRoomsTestAccess::PostLogin(Server, Stranger, TEXT("203.0.113.5:7777?rsv=rsv_forged"));
	TestEqual(TEXT("neither reaches the roster"), Server->GetRoomPlayerCount(TEXT("blob-7a3f")), 0);

	// Decision B's second half: with fail-open on, a ticketless login is admitted to the GAME and
	// reported to NOBODY — no frame, no roster entry — because no platform ticket named a player.
	FPlayServRoomsTestAccess::SetAdmissionFailOpen(Server, true);
	const int32 FramesBeforeFailOpen = Socket->CountSentOfType(TEXT("room_presence"));
	APlayerController* Local = NewObject<APlayerController>(GetTransientPackage());
	Server->VerifyTicket(FString());
	FPlayServRoomsTestAccess::PostLogin(Server, Local, TEXT("203.0.113.5:7777?Name=Dev"));
	TestEqual(TEXT("fail-open sends no presence frame"), Socket->CountSentOfType(TEXT("room_presence")), FramesBeforeFailOpen);
	TestEqual(TEXT("and puts nobody on the roster"), Server->GetRoomPlayerCount(TEXT("blob-7a3f")), 0);
	TestTrue(TEXT("and the connection maps to no player id"), Server->GetPlayerId(Local).IsEmpty());
	FPlayServRoomsTestAccess::SetAdmissionFailOpen(Server, false);

	// The real path: a pushed ticket, PreLogin's verdict, then the engine's login.
	Socket->SimulateMessage(TEXT("{\"type\":\"ticket_offer\",\"reservation_token\":\"rsv_1\",\"room_name\":\"blob-7a3f\",\"player_id\":\"plr_a\",\"expires_in\":10}"));
	Server->VerifyTicket(TEXT("rsv_1"));
	FPlayServRoomsTestAccess::PostLogin(Server, Player, TEXT("203.0.113.5:7777?rsv=rsv_1"));
	TSharedPtr<FJsonObject> Join = Socket->LastSentOfType(TEXT("room_presence"));
	TestTrue(TEXT("the join frame carries the token that redeems it"),
		Join.IsValid() && Join->GetStringField(TEXT("event")) == TEXT("join") && Join->GetStringField(TEXT("reservation_token")) == TEXT("rsv_1"));
	TestTrue(TEXT("the player is on the roster"), FPlayServRoomsTestAccess::RosterContains(Server, TEXT("blob-7a3f"), TEXT("plr_a")));
	TestEqual(TEXT("the verdict was consumed by the login"), FPlayServRoomsTestAccess::VerifiedCount(Server), 0);

	// A drop is not a departure: the seat is parked, not reported.
	const int32 FramesAfterJoin = Socket->CountSentOfType(TEXT("room_presence"));
	FPlayServRoomsTestAccess::Logout(Server, Player);
	Clock.Now += 10.0;
	Socket->SimulateMessage(TEXT("{\"type\":\"ping\"}"));
	FPlayServRoomsTestAccess::Tick(Server, Clock.Now);
	TestTrue(TEXT("still a member inside the grace"), FPlayServRoomsTestAccess::RosterContains(Server, TEXT("blob-7a3f"), TEXT("plr_a")));
	TestTrue(TEXT("the park sent no frame"), FPlayServRoomsTestAccess::IsParked(Server, TEXT("blob-7a3f"), TEXT("plr_a")));
	TestEqual(TEXT("no leave frame while parked"), Socket->CountSentOfType(TEXT("room_presence")), FramesAfterJoin);

	// The player comes back with a fresh ticket: the login cancels the park.
	Socket->SimulateMessage(TEXT("{\"type\":\"ticket_offer\",\"reservation_token\":\"rsv_again\",\"room_name\":\"blob-7a3f\",\"player_id\":\"plr_a\",\"expires_in\":10}"));
	Server->VerifyTicket(TEXT("rsv_again"));
	APlayerController* Returning = NewObject<APlayerController>(GetTransientPackage());
	FPlayServRoomsTestAccess::PostLogin(Server, Returning, TEXT("203.0.113.5:7777?rsv=rsv_again"));
	TestFalse(TEXT("the park is cancelled by the return"), FPlayServRoomsTestAccess::IsParked(Server, TEXT("blob-7a3f"), TEXT("plr_a")));
	TestEqual(TEXT("still one member, not two"), Server->GetRoomPlayerCount(TEXT("blob-7a3f")), 1);

	// This time the grace lapses: the leave goes out and OnPlayerRemoved fires.
	UPlayServRoomsEventSpy* Spy = NewObject<UPlayServRoomsEventSpy>(GetTransientPackage());
	Server->OnPlayerRemoved.AddDynamic(Spy, &UPlayServRoomsEventSpy::OnPlayerRemoved);
	const int32 FramesBeforeLapse = Socket->CountSentOfType(TEXT("room_presence"));
	FPlayServRoomsTestAccess::Logout(Server, Returning);
	for (int32 Step = 0; Step < 4; ++Step)
	{
		Clock.Now += 8.0;
		Socket->SimulateMessage(TEXT("{\"type\":\"ping\"}"));
		FPlayServRoomsTestAccess::Tick(Server, Clock.Now);
	}
	TestFalse(TEXT("the grace lapse removes the member"), FPlayServRoomsTestAccess::RosterContains(Server, TEXT("blob-7a3f"), TEXT("plr_a")));
	TSharedPtr<FJsonObject> Leave = Socket->LastSentOfType(TEXT("room_presence"));
	TestTrue(TEXT("exactly one leave frame, at the removal decision"),
		Socket->CountSentOfType(TEXT("room_presence")) == FramesBeforeLapse + 1 && Leave->GetStringField(TEXT("event")) == TEXT("leave"));
	TestTrue(TEXT("OnPlayerRemoved names the player and why"), Spy->WasRemoved(TEXT("plr_a"), TEXT("reconnect_grace_lapsed")));
	Server->OnPlayerRemoved.RemoveDynamic(Spy, &UPlayServRoomsEventSpy::OnPlayerRemoved);

	FPlayServRoomsTestAccess::End(Server);
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Rooms.Travel.HardTravelResumesWithTheSameTicket
//
// A non-seamless travel reconnects every client with its old travel URL — TRAVEL_Relative keeps
// `?rsv=` — so PreLogin meets the ticket that was redeemed when the player first joined. For a
// member the room still holds whom no controller stands for, that ticket resumes the seat, with no
// join frame: the platform never saw the player leave, and it refuses a second join with a
// redeemed ticket. A member who is connected is refused as before, so a copied ticket cannot take
// a live seat; a member who left is gone. The room keeps the ticket for as long as the member is in
// it, so a travel long after the admission table forgot it resumes too (PSV-2811).
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServRoomsHardTravelTest,
	"PlayServ.Rooms.Travel.HardTravelResumesWithTheSameTicket",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServRoomsHardTravelTest::RunTest(const FString& Parameters)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!TestNotNull(TEXT("subsystem"), PS))
	{
		return false;
	}
	UPlayServRooms* Server = PS->GetRooms();

	FFakeClock Clock;
	FPlayServFakeUplinkFactory Factory;
	TSharedPtr<FPlayServFakeUplinkTransport> Socket = BeginHostingOneRoom(Server, Factory, Clock, TEXT("blob-7a3f"));
	FScriptedConnections Connections;
	Connections.Install(Server);

	const FString TravelUrl = TEXT("203.0.113.5:7777?rsv=rsv_1");
	APlayerController* First = AdmitMember(Server, *Socket, Connections, TEXT("blob-7a3f"), TEXT("plr_a"), TEXT("rsv_1"));
	const int32 Frames = Socket->CountSentOfType(TEXT("room_presence"));
	TestEqual(TEXT("the first login joined with the ticket"), Frames, 1);

	// The travel closes the connection; the client comes back with the same URL.
	Connections.Close(First);
	FPlayServRoomsTestAccess::Logout(Server, First);
	TestTrue(TEXT("the travel parks the seat"), FPlayServRoomsTestAccess::IsParked(Server, TEXT("blob-7a3f"), TEXT("plr_a")));
	const FPlayServTicketVerdict Back = Server->VerifyTicket(TEXT("rsv_1"));
	TestTrue(TEXT("the ticket the member was admitted with resumes their seat"), Back.bAccepted && Back.PlayerId == TEXT("plr_a") && Back.RoomName == TEXT("blob-7a3f"));
	APlayerController* Second = NewObject<APlayerController>(GetTransientPackage());
	Connections.Open(Second, TravelUrl);
	FPlayServRoomsTestAccess::PostLogin(Server, Second, TravelUrl);
	TestFalse(TEXT("the resume cancels the park"), FPlayServRoomsTestAccess::IsParked(Server, TEXT("blob-7a3f"), TEXT("plr_a")));
	TestEqual(TEXT("and sends no frame — the platform never saw the player leave"), Socket->CountSentOfType(TEXT("room_presence")), Frames);
	TestEqual(TEXT("one member, not two"), Server->GetRoomPlayerCount(TEXT("blob-7a3f")), 1);
	TestEqual(TEXT("the new connection maps to the member"), Server->GetPlayerId(Second), FString(TEXT("plr_a")));

	// The member is connected: a second connection presenting their ticket is refused as before.
	const FPlayServTicketVerdict Copy = Server->VerifyTicket(TEXT("rsv_1"));
	TestTrue(TEXT("a connected member's ticket is refused reservation_consumed"), !Copy.bAccepted && Copy.Reason == TEXT("reservation_consumed"));
	TestEqual(TEXT("and the refusal leaves nothing waiting for a login"), FPlayServRoomsTestAccess::VerifiedCount(Server), 0);

	// 500 s on, long after the admission table forgot the redeemed ticket, the room still knows it.
	for (int32 Step = 0; Step < 50; ++Step)
	{
		Clock.Now += 10.0;
		TickAt(Server, *Socket, Clock.Now);
	}
	TestEqual(TEXT("the admission table has dropped the redeemed ticket"), FPlayServRoomsTestAccess::TicketCount(Server), 0);
	Connections.Close(Second);
	FPlayServRoomsTestAccess::Logout(Server, Second);
	const FPlayServTicketVerdict Later = Server->VerifyTicket(TEXT("rsv_1"));
	TestTrue(TEXT("a travel 500 s into the match resumes as well"), Later.bAccepted && Later.PlayerId == TEXT("plr_a"));
	APlayerController* Third = NewObject<APlayerController>(GetTransientPackage());
	Connections.Open(Third, TravelUrl);
	FPlayServRoomsTestAccess::PostLogin(Server, Third, TravelUrl);
	TestEqual(TEXT("still no frame"), Socket->CountSentOfType(TEXT("room_presence")), Frames);
	TestEqual(TEXT("and the connection maps to the member"), Server->GetPlayerId(Third), FString(TEXT("plr_a")));

	// The member leaves for good: the grace lapses, the leave is reported, the ticket admits nobody.
	Connections.Close(Third);
	FPlayServRoomsTestAccess::Logout(Server, Third);
	for (int32 Step = 0; Step < 4; ++Step)
	{
		Clock.Now += 8.0;
		TickAt(Server, *Socket, Clock.Now);
	}
	TestFalse(TEXT("the lapsed grace removes the member"), FPlayServRoomsTestAccess::RosterContains(Server, TEXT("blob-7a3f"), TEXT("plr_a")));
	const FPlayServTicketVerdict Gone = Server->VerifyTicket(TEXT("rsv_1"));
	TestTrue(TEXT("a departed member's ticket is refused"), !Gone.bAccepted && Gone.Reason == TEXT("reservation_invalid"));

	FPlayServRoomsTestAccess::End(Server);
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Rooms.Travel.SwappedControllerKeepsTheSeat
//
// A seamless travel keeps each connection but hands it to a new controller when the next map's
// controller class differs (AGameModeBase::SwapPlayerControllers). The old one is destroyed with
// no logout — its PlayerState moved on — so the module cannot follow the player by controller; it
// follows the ticket the connection carries. The new controller resolves to the same player at
// once, the lost one does not release the seat, and the new one's own drop is parked and reported
// like any other. If the new controller is gone before the module has seen it, the lost one alone
// still parks the seat. And a player connected through a second controller keeps the seat when
// the first one drops (PSV-2811).
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServRoomsSwappedControllerTest,
	"PlayServ.Rooms.Travel.SwappedControllerKeepsTheSeat",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServRoomsSwappedControllerTest::RunTest(const FString& Parameters)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!TestNotNull(TEXT("subsystem"), PS))
	{
		return false;
	}
	UPlayServRooms* Server = PS->GetRooms();

	FFakeClock Clock;
	FPlayServFakeUplinkFactory Factory;
	TSharedPtr<FPlayServFakeUplinkTransport> Socket = BeginHostingOneRoom(Server, Factory, Clock, TEXT("blob-7a3f"));
	FScriptedConnections Connections;
	Connections.Install(Server);
	UPlayServRoomsEventSpy* Spy = NewObject<UPlayServRoomsEventSpy>(GetTransientPackage());
	Server->OnPlayerRemoved.AddDynamic(Spy, &UPlayServRoomsEventSpy::OnPlayerRemoved);

	// plr_a: the swap, seen by the module before the new controller drops.
	APlayerController* Old = AdmitMember(Server, *Socket, Connections, TEXT("blob-7a3f"), TEXT("plr_a"), TEXT("rsv_1"));
	const int32 Frames = Socket->CountSentOfType(TEXT("room_presence"));
	APlayerController* Swapped = NewObject<APlayerController>(GetTransientPackage());
	Connections.Close(Old);
	Connections.Open(Swapped, TEXT("203.0.113.5:7777?rsv=rsv_1"));
	TestEqual(TEXT("the swapped-in controller resolves to the same player at once"), Server->GetPlayerId(Swapped), FString(TEXT("plr_a")));
	Old->MarkAsGarbage();
	Clock.Now += 1.0;
	TickAt(Server, *Socket, Clock.Now);
	TestFalse(TEXT("the lost controller does not release the seat"), FPlayServRoomsTestAccess::IsParked(Server, TEXT("blob-7a3f"), TEXT("plr_a")));
	TestEqual(TEXT("and nothing is reported"), Socket->CountSentOfType(TEXT("room_presence")), Frames);
	TestEqual(TEXT("the player still resolves"), Server->GetPlayerId(Swapped), FString(TEXT("plr_a")));
	Connections.Close(Swapped);
	FPlayServRoomsTestAccess::Logout(Server, Swapped);
	TestTrue(TEXT("the swapped-in controller's drop parks the seat"), FPlayServRoomsTestAccess::IsParked(Server, TEXT("blob-7a3f"), TEXT("plr_a")));

	// plr_b: the new controller is gone before the module has seen it; its drop cannot be placed, the lost controller parks the seat.
	APlayerController* OldB = AdmitMember(Server, *Socket, Connections, TEXT("blob-7a3f"), TEXT("plr_b"), TEXT("rsv_2"));
	APlayerController* SwappedB = NewObject<APlayerController>(GetTransientPackage());
	Connections.Close(OldB);
	Connections.Open(SwappedB, TEXT("203.0.113.5:7777?rsv=rsv_2"));
	OldB->MarkAsGarbage();
	Connections.Close(SwappedB);
	FPlayServRoomsTestAccess::Logout(Server, SwappedB);
	Clock.Now += 1.0;
	TickAt(Server, *Socket, Clock.Now);
	TestTrue(TEXT("the lost controller parks the seat on the next sweep"), FPlayServRoomsTestAccess::IsParked(Server, TEXT("blob-7a3f"), TEXT("plr_b")));

	// Both graces lapse: each departure is reported once.
	for (int32 Step = 0; Step < 4; ++Step)
	{
		Clock.Now += 8.0;
		TickAt(Server, *Socket, Clock.Now);
	}
	TestTrue(TEXT("plr_a's departure is reported"), !FPlayServRoomsTestAccess::RosterContains(Server, TEXT("blob-7a3f"), TEXT("plr_a")) && Spy->WasRemoved(TEXT("plr_a"), TEXT("reconnect_grace_lapsed")));
	TestTrue(TEXT("plr_b's departure is reported"), !FPlayServRoomsTestAccess::RosterContains(Server, TEXT("blob-7a3f"), TEXT("plr_b")) && Spy->WasRemoved(TEXT("plr_b"), TEXT("reconnect_grace_lapsed")));
	TestEqual(TEXT("one leave frame each"), Socket->CountSentOfType(TEXT("room_presence")), Frames + 1 + 2);

	// plr_c joins again from a second connection before the first one times out; the first one's drop keeps the seat.
	APlayerController* FirstC = AdmitMember(Server, *Socket, Connections, TEXT("blob-7a3f"), TEXT("plr_c"), TEXT("rsv_3"));
	APlayerController* SecondC = AdmitMember(Server, *Socket, Connections, TEXT("blob-7a3f"), TEXT("plr_c"), TEXT("rsv_4"));
	Connections.Close(FirstC);
	FPlayServRoomsTestAccess::Logout(Server, FirstC);
	TestFalse(TEXT("a drop while another controller stands for the player keeps the seat"), FPlayServRoomsTestAccess::IsParked(Server, TEXT("blob-7a3f"), TEXT("plr_c")));
	Connections.Close(SecondC);
	FPlayServRoomsTestAccess::Logout(Server, SecondC);
	TestTrue(TEXT("the last controller's drop parks it"), FPlayServRoomsTestAccess::IsParked(Server, TEXT("blob-7a3f"), TEXT("plr_c")));

	Server->OnPlayerRemoved.RemoveDynamic(Spy, &UPlayServRoomsEventSpy::OnPlayerRemoved);
	FPlayServRoomsTestAccess::End(Server);
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Rooms.Travel.MapLoadPausesTheGrace
//
// A non-seamless travel blocks the game thread for as long as the next map takes to load, so the
// grace of every seat the travel parked would lapse on the first tick after a slow load — before a
// client could have come back. The grace does not run while a map loads. The load also tears the
// old world down, and a controller that goes with it without a logout parks its seat once the map
// is in (PSV-2811).
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServRoomsMapLoadGraceTest,
	"PlayServ.Rooms.Travel.MapLoadPausesTheGrace",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServRoomsMapLoadGraceTest::RunTest(const FString& Parameters)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!TestNotNull(TEXT("subsystem"), PS))
	{
		return false;
	}
	UPlayServRooms* Server = PS->GetRooms();

	FFakeClock Clock;
	FPlayServFakeUplinkFactory Factory;
	TSharedPtr<FPlayServFakeUplinkTransport> Socket = BeginHostingOneRoom(Server, Factory, Clock, TEXT("blob-7a3f"));
	FScriptedConnections Connections;
	Connections.Install(Server);
	APlayerController* PlayerA = AdmitMember(Server, *Socket, Connections, TEXT("blob-7a3f"), TEXT("plr_a"), TEXT("rsv_1"));
	APlayerController* PlayerB = AdmitMember(Server, *Socket, Connections, TEXT("blob-7a3f"), TEXT("plr_b"), TEXT("rsv_2"));

	// The travel: plr_a's connection closes with a logout before the load, plr_b's controller goes with the old world.
	Connections.Close(PlayerA);
	FPlayServRoomsTestAccess::Logout(Server, PlayerA);
	Clock.Now += 5.0;
	FPlayServRoomsTestAccess::MapLoadStarted(Server);
	Connections.Close(PlayerB);
	PlayerB->MarkAsGarbage();
	Clock.Now += 40.0;
	FPlayServRoomsTestAccess::MapLoaded(Server);
	TestTrue(TEXT("a controller the load took without a logout parks its seat"), FPlayServRoomsTestAccess::IsParked(Server, TEXT("blob-7a3f"), TEXT("plr_b")));

	Clock.Now += 20.0;
	TickAt(Server, *Socket, Clock.Now);
	TestTrue(TEXT("65 s after the drop, 40 of them loading, the seat is still held"), FPlayServRoomsTestAccess::IsParked(Server, TEXT("blob-7a3f"), TEXT("plr_a")));
	TestTrue(TEXT("and so is the one the load took"), FPlayServRoomsTestAccess::IsParked(Server, TEXT("blob-7a3f"), TEXT("plr_b")));

	Clock.Now += 15.0;
	TickAt(Server, *Socket, Clock.Now);
	TestFalse(TEXT("the grace runs again once the map is in"), FPlayServRoomsTestAccess::RosterContains(Server, TEXT("blob-7a3f"), TEXT("plr_a")));
	TestFalse(TEXT("for both"), FPlayServRoomsTestAccess::RosterContains(Server, TEXT("blob-7a3f"), TEXT("plr_b")));

	FPlayServRoomsTestAccess::End(Server);
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Rooms.Client.BrowseQueryAndPage
//
// The browse request the SDK composes and the page it reads back. The attributes filter goes as
// dot notation, which is what the server parses — the contract's old `deepObject` spelling never
// matched it (UE finding F1, closed by Story 15.18).
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServRoomsClientBrowseTest,
	"PlayServ.Rooms.Client.BrowseQueryAndPage",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServRoomsClientBrowseTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("no filter is no query"), PlayServRoomsClientWire::BuildBrowseQuery(FPlayServRoomFilters()), FString());

	FPlayServRoomFilters Filters;
	Filters.PlacementState = EPlayServPlacementState::Open;
	Filters.Region = TEXT("eu-west-1");
	Filters.Attributes.Add(TEXT("map"), TEXT("arena"));
	Filters.Attributes.Add(TEXT("mode"), TEXT("free for all"));
	Filters.Cursor = TEXT("eyJvIjo1MH0=");
	TestEqual(TEXT("placement, region, dot-notation attributes and the cursor, url-encoded"),
		PlayServRoomsClientWire::BuildBrowseQuery(Filters),
		FString(TEXT("?placement_state=open&region=eu-west-1&attributes.map=arena&attributes.mode=free%20for%20all&cursor=eyJvIjo1MH0%3D")));

	FPlayServRoomFilters Unfiltered;
	Unfiltered.PlacementState = EPlayServPlacementState::Unknown;
	TestEqual(TEXT("Unknown is not a wire state, so it filters nothing"), PlayServRoomsClientWire::BuildBrowseQuery(Unfiltered), FString());

	const FString PageJson = TEXT("{\"data\":[")
		TEXT("{\"room_name\":\"blob-7a3f\",\"players\":3,\"capacity\":16,\"state\":\"lobby\",\"placement_state\":\"open\",")
		TEXT("\"region\":\"eu-west-1\",\"attributes\":{\"name\":\"Blob Arena\",\"round\":3,\"ranked\":true,\"tags\":[\"a\",\"b\"]},")
		TEXT("\"connect\":{\"host\":\"203.0.113.5\",\"port\":7777,\"transport\":\"udp\",\"connect_string\":null,\"region\":\"eu-west-1\"}},")
		TEXT("{\"room_name\":\"blob-pending\",\"players\":0,\"capacity\":16,\"state\":null,\"placement_state\":\"open\",\"connect\":null}")
		TEXT("],\"page\":{\"cursor_next\":\"eyJvIjo1MH0=\",\"cursor_prev\":null,\"has_more\":true}}");

	FPlayServBrowsePage Page;
	TestTrue(TEXT("the page parses"), PlayServRoomsClientWire::ParseBrowsePage(FPlayServFakeUplinkTransport::ParseJson(PageJson), Page));
	TestEqual(TEXT("two rooms"), Page.Rooms.Num(), 2);
	TestEqual(TEXT("cursor_next becomes NextCursor"), Page.NextCursor, FString(TEXT("eyJvIjo1MH0=")));
	TestTrue(TEXT("has_more"), Page.bHasMore);

	const FPlayServRoomListing& First = Page.Rooms[0];
	TestEqual(TEXT("room name"), First.RoomName, FString(TEXT("blob-7a3f")));
	TestEqual(TEXT("players"), First.Players, 3);
	TestEqual(TEXT("capacity"), First.Capacity, 16);
	TestTrue(TEXT("placement state"), First.PlacementState == EPlayServPlacementState::Open);
	TestEqual(TEXT("connect host"), First.Connect.Host, FString(TEXT("203.0.113.5")));
	TestEqual(TEXT("connect port"), First.Connect.Port, 7777);
	TestEqual(TEXT("a string attribute is verbatim"), First.Attributes[TEXT("name")], FString(TEXT("Blob Arena")));
	TestEqual(TEXT("a number attribute takes its string form"), First.Attributes[TEXT("round")], FString(TEXT("3")));
	TestEqual(TEXT("a bool attribute too"), First.Attributes[TEXT("ranked")], FString(TEXT("true")));
	TestEqual(TEXT("a container attribute arrives as compact JSON"), First.Attributes[TEXT("tags")], FString(TEXT("[\"a\",\"b\"]")));

	// A room whose server has not registered yet is listed with no address — the client sees the
	// room it is about to be admitted to (ADR-005 §4).
	TestFalse(TEXT("a reservation-only room carries no address"), Page.Rooms[1].Connect.IsSet());

	FPlayServBrowsePage Empty;
	TestTrue(TEXT("an empty page is still a page"), PlayServRoomsClientWire::ParseBrowsePage(
		FPlayServFakeUplinkTransport::ParseJson(TEXT("{\"data\":[],\"page\":{\"cursor_next\":null,\"has_more\":false}}")), Empty));
	TestEqual(TEXT("no rooms"), Empty.Rooms.Num(), 0);
	TestTrue(TEXT("no cursor"), Empty.NextCursor.IsEmpty() && !Empty.bHasMore);

	FPlayServBrowsePage Malformed;
	TestFalse(TEXT("an answer with no data array is refused"), PlayServRoomsClientWire::ParseBrowsePage(
		FPlayServFakeUplinkTransport::ParseJson(TEXT("{\"page\":{}}")), Malformed));
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Rooms.Client.JoinRoomTicketAndTravelUrl
//
// The `matched` shape into a ticket, and the ticket into a travel URL. The lifetime is counted
// on a monotonic clock from `expires_in`; when the platform does not serve it, from `expires_at`
// minus the response's own `Date` header — never the player's wall clock (UE finding F2).
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServRoomsClientJoinRoomTest,
	"PlayServ.Rooms.Client.JoinRoomTicketAndTravelUrl",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServRoomsClientJoinRoomTest::RunTest(const FString& Parameters)
{
	const FString MatchedJson = TEXT("{\"status\":\"matched\",\"room_name\":\"blob-7a3f\",\"reservation_token\":\"rsv_abc\",")
		TEXT("\"expires_at\":\"2026-09-13T12:00:10Z\",\"expires_in\":10,")
		TEXT("\"connect\":{\"host\":\"203.0.113.5\",\"port\":7777,\"transport\":\"udp\"},")
		TEXT("\"region\":\"eu-west-1\",\"attributes\":{\"map\":\"arena\"}}");

	FPlayServRoomTicket Ticket;
	TestTrue(TEXT("the matched shape parses"), PlayServRoomsClientWire::ParseMatched(
		FPlayServFakeUplinkTransport::ParseJson(MatchedJson), FString(), 1000.0, Ticket));
	TestEqual(TEXT("room name"), Ticket.RoomName, FString(TEXT("blob-7a3f")));
	TestEqual(TEXT("token"), Ticket.ReservationToken, FString(TEXT("rsv_abc")));
	TestEqual(TEXT("expires_in is what the client counts"), Ticket.ExpiresInSeconds, 10);
	TestEqual(TEXT("the deadline is monotonic, from receipt"), Ticket.ExpiresAtMonotonic, 1010.0);
	TestEqual(TEXT("region"), Ticket.Region, FString(TEXT("eu-west-1")));
	TestEqual(TEXT("attributes"), Ticket.Attributes[TEXT("map")], FString(TEXT("arena")));
	TestFalse(TEXT("not expired at receipt"), Ticket.IsExpired(1000.0));
	TestTrue(TEXT("expired once the deadline passes"), Ticket.IsExpired(1010.0));

	// The fallback: no `expires_in`, so the platform's own clock (the Date header) sets the TTL.
	const FString NoExpiresIn = TEXT("{\"status\":\"matched\",\"room_name\":\"blob-7a3f\",\"reservation_token\":\"rsv_abc\",")
		TEXT("\"expires_at\":\"2026-09-13T12:00:10Z\",\"connect\":{\"host\":\"203.0.113.5\",\"port\":7777,\"transport\":\"udp\"}}");
	FPlayServRoomTicket Fallback;
	TestTrue(TEXT("the fallback parses"), PlayServRoomsClientWire::ParseMatched(
		FPlayServFakeUplinkTransport::ParseJson(NoExpiresIn), TEXT("Sat, 13 Sep 2026 12:00:00 GMT"), 500.0, Fallback));
	TestEqual(TEXT("expires_at minus the Date header, not the local clock"), Fallback.ExpiresInSeconds, 10);
	TestEqual(TEXT("still a monotonic deadline"), Fallback.ExpiresAtMonotonic, 510.0);

	FPlayServRoomTicket NoClock;
	PlayServRoomsClientWire::ParseMatched(FPlayServFakeUplinkTransport::ParseJson(NoExpiresIn), FString(), 500.0, NoClock);
	TestEqual(TEXT("with neither expires_in nor a Date header the ticket is treated as already due"), NoClock.ExpiresInSeconds, 0);

	FPlayServRoomTicket NoToken;
	TestFalse(TEXT("an answer with no reservation token is refused"), PlayServRoomsClientWire::ParseMatched(
		FPlayServFakeUplinkTransport::ParseJson(TEXT("{\"status\":\"matched\",\"room_name\":\"x\"}")), FString(), 0.0, NoToken));

	// The travel URL: `?`-delimited, no JWT, no player id.
	TestEqual(TEXT("host:port?rsv="), UPlayServRooms::BuildTravelUrl(Ticket), FString(TEXT("203.0.113.5:7777?rsv=rsv_abc")));

	FPlayServRoomTicket WithConnectString = Ticket;
	WithConnectString.Connect.ConnectString = TEXT("steam.123456789");
	TestEqual(TEXT("connect_string replaces host:port"), UPlayServRooms::BuildTravelUrl(WithConnectString), FString(TEXT("steam.123456789?rsv=rsv_abc")));

	FPlayServRoomTicket Addressless = Ticket;
	Addressless.Connect = FPlayServRoomConnect();
	TestTrue(TEXT("a ticket with no address has no travel URL"), UPlayServRooms::BuildTravelUrl(Addressless).IsEmpty());
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Rooms.Client.JoinRoomLoop
//
// The join loop's rules, as pure decisions. A room-directed join asks ONE room, so the room's own
// answers are terminal: only a `503 room_unreachable` — the room's instance failing to answer its
// ticket_offer in time — is worth asking again. A matched room with no address yet is re-polled
// until it publishes one or the connect wait runs out.
//
// There is no `:find` loop to test: the SDK has no matchmaking entry point (owner decision,
// 2026-09-13).
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServRoomsClientJoinLoopTest,
	"PlayServ.Rooms.Client.JoinRoomLoop",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServRoomsClientJoinLoopTest::RunTest(const FString& Parameters)
{
	using namespace PlayServRoomsClientWire;

	FJoinLoopState State;

	// A usable ticket is delivered at once.
	FJoinAnswer Joined;
	Joined.bSuccess = true;
	Joined.bHasTicket = true;
	Joined.bTicketHasConnect = true;
	TestTrue(TEXT("a join with an address delivers"), DecideAfterJoin(State, Joined).Step == EJoinStep::Deliver);

	// A room whose server has not registered yet is re-polled by name.
	Joined.bTicketHasConnect = false;
	FJoinStepDecision Decision = DecideAfterJoin(State, Joined);
	TestTrue(TEXT("a join without an address re-polls the same room"), Decision.Step == EJoinStep::JoinNamedRoom);
	TestEqual(TEXT("after the poll interval"), Decision.DelaySeconds, ConnectPollIntervalSeconds);
	TestFalse(TEXT("waiting for an address is not a retry"), Decision.bSpendsRetry);

	// …until the connect wait runs out.
	FJoinLoopState Waited = State;
	Waited.ConnectWaitedSeconds = ConnectWaitSeconds;
	Decision = DecideAfterJoin(Waited, Joined);
	TestTrue(TEXT("a room that never publishes an address gives up"), Decision.Step == EJoinStep::Stop);
	TestEqual(TEXT("typed room_unreachable"), Decision.Error.ProblemCode, FString(TEXT("room_unreachable")));

	// A 2xx with no token is contract drift, not a silent retry.
	Joined.bHasTicket = false;
	TestTrue(TEXT("a join answered with no reservation token is contract drift"),
		DecideAfterJoin(State, Joined).Error.Code == EPlayServErrorCode::ContractMismatch);

	// The room's own refusals are terminal — the player asked for THAT room.
	const TCHAR* Terminal[] = { TEXT("room_not_found"), TEXT("room_full"), TEXT("room_closed"), TEXT("room_refused"), TEXT("room_type_not_found") };
	for (const TCHAR* Code : Terminal)
	{
		FJoinAnswer Refused;
		Refused.bSuccess = false;
		Refused.ProblemCode = Code;
		Refused.Error = FPlayServError::Make(EPlayServErrorCode::Conflict, Code);
		const FJoinStepDecision Stop = DecideAfterJoin(State, Refused);
		TestTrue(FString::Printf(TEXT("%s is terminal"), Code), Stop.Step == EJoinStep::Stop);
		TestEqual(FString::Printf(TEXT("%s keeps the platform's own message"), Code), Stop.Error.Message, FString(Code));
	}

	// A stalled room instance is the one retryable refusal, after the Retry-After it asked for.
	FJoinAnswer Unreachable;
	Unreachable.bSuccess = false;
	Unreachable.ProblemCode = TEXT("room_unreachable");
	Unreachable.RetryAfterSeconds = 3;
	Unreachable.Error = FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("room_unreachable"));
	Decision = DecideAfterJoin(State, Unreachable);
	TestTrue(TEXT("room_unreachable is retried on the same room"), Decision.Step == EJoinStep::JoinNamedRoom);
	TestEqual(TEXT("after the platform's Retry-After"), Decision.DelaySeconds, 3.0);
	TestTrue(TEXT("and it spends a retry"), Decision.bSpendsRetry);

	FJoinLoopState Spent = State;
	Spent.Retries = MaxRetries;
	TestTrue(TEXT("past MaxRetries it stops"), DecideAfterJoin(Spent, Unreachable).Step == EJoinStep::Stop);

	FJoinLoopState Late = State;
	Late.ElapsedSeconds = JoinTimeoutSeconds;
	TestTrue(TEXT("a spent budget stops even a retryable refusal"), DecideAfterJoin(Late, Unreachable).Step == EJoinStep::Stop);

	// A transport failure carries its own error out.
	FJoinAnswer Failed;
	Failed.bSuccess = false;
	Failed.Error = FPlayServError::Make(EPlayServErrorCode::NetworkUnreachable, TEXT("no route to host"));
	Decision = DecideAfterJoin(State, Failed);
	TestTrue(TEXT("a transport failure stops the join"), Decision.Step == EJoinStep::Stop);
	TestEqual(TEXT("with its own error"), Decision.Error.Message, FString(TEXT("no route to host")));
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Rooms.Client.RefusesWithoutAPlayerSession
//
// Browse and join name a player, and only a client session can: pk_* alone cannot.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServRoomsClientSessionTest,
	"PlayServ.Rooms.Client.RefusesWithoutAPlayerSession",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServRoomsClientSessionTest::RunTest(const FString& Parameters)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!TestNotNull(TEXT("subsystem"), PS))
	{
		return false;
	}
	UPlayServRooms* Client = PS->GetRooms();
	PS->GetAuth()->Logout(FPlayServSimpleCallback());

	bool bBrowsed = true;
	FPlayServError BrowseError;
	Client->Browse(TEXT("blob-arena"), FPlayServRoomFilters(), FPlayServBrowseCallback::CreateLambda(
		[&bBrowsed, &BrowseError](bool bSuccess, const FPlayServBrowsePage&, const FPlayServError& Error)
		{
			bBrowsed = bSuccess;
			BrowseError = Error;
		}));
	TestFalse(TEXT("browse refuses without a player"), bBrowsed);
	TestTrue(TEXT("typed Unauthorized"), BrowseError.Code == EPlayServErrorCode::Unauthorized);

	bool bJoined = true;
	Client->JoinRoom(TEXT("blob-arena"), TEXT("blob-7a3f"), nullptr, FPlayServJoinCallback::CreateLambda(
		[&bJoined](bool bSuccess, const FPlayServJoinResult&, const FPlayServError&)
		{
			bJoined = bSuccess;
		}));
	TestFalse(TEXT("JoinRoom refuses without a player"), bJoined);

	bool bCreated = true;
	FPlayServError CreateError;
	TMap<FString, FString> Attributes;
	Attributes.Add(TEXT("map"), TEXT("BlobArenaMap"));
	FPlayServRoomsTestAccess::RequestNewRoomOfType(Client, TEXT("blob-arena"), Attributes, FPlayServRequestNewRoomCallback::CreateLambda(
		[&bCreated, &CreateError](bool bSuccess, const FPlayServRoomListing&, const FPlayServError& Error)
		{
			bCreated = bSuccess;
			CreateError = Error;
		}));
	TestFalse(TEXT("RequestNewRoom refuses without a player — nothing may start a server for nobody"), bCreated);
	TestTrue(TEXT("typed Unauthorized"), CreateError.Code == EPlayServErrorCode::Unauthorized);

	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Rooms.Client.RequestNewRoomChecksTheReservedCapacity  (PSV-2707)
//
// The host's seat count rides as the reserved capacity attribute. An unusable one is refused on
// the spot — before the session is even consulted — rather than sent, starting a server whose room
// would then quietly fall back to the room configuration's capacity.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServRoomsRequestCapacityTest,
	"PlayServ.Rooms.Client.RequestNewRoomChecksTheReservedCapacity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServRoomsRequestCapacityTest::RunTest(const FString& Parameters)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!TestNotNull(TEXT("subsystem"), PS))
	{
		return false;
	}
	UPlayServRooms* Client = PS->GetRooms();
	PS->GetAuth()->Logout(FPlayServSimpleCallback());

	for (const TCHAR* Unusable : { TEXT("0"), TEXT("1001"), TEXT("six"), TEXT("-3"), TEXT("") })
	{
		TMap<FString, FString> Attributes;
		Attributes.Add(PlayServ::Rooms::Attributes::Capacity, Unusable);
		FPlayServError Error;
		FPlayServRoomsTestAccess::RequestNewRoomOfType(Client, TEXT("blob-arena"), Attributes, FPlayServRequestNewRoomCallback::CreateLambda([&Error](bool, const FPlayServRoomListing&, const FPlayServError& InError) { Error = InError; }));
		TestEqual(FString::Printf(TEXT("capacity '%s' is refused as invalid input"), Unusable), Error.Code, EPlayServErrorCode::ValidationFailed);
		TestTrue(FString::Printf(TEXT("capacity '%s': the refusal names the capacity"), Unusable), Error.Message.Contains(TEXT("capacity")));
	}

	TMap<FString, FString> Sized;
	Sized.Add(PlayServ::Rooms::Attributes::Capacity, TEXT("6"));
	FPlayServError PastInput;
	FPlayServRoomsTestAccess::RequestNewRoomOfType(Client, TEXT("blob-arena"), Sized, FPlayServRequestNewRoomCallback::CreateLambda([&PastInput](bool, const FPlayServRoomListing&, const FPlayServError& InError) { PastInput = InError; }));
	TestEqual(TEXT("a usable capacity passes the input check and meets the session check"), PastInput.Code, EPlayServErrorCode::Unauthorized);
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Rooms.Close.BodylessPostCarriesContentLength
//
// REGRESSION. `POST /rooms/{slug}/{room}:close` is the SDK's one write with no body, and UE's
// curl backend does NOT set Content-Length for an empty payload unless
// `http.AlwaysSetContentLengthEnabled` is on — which it is not by default (UE 5.8
// CurlHttp.cpp). The platform answers such a request `411 Length Required`, so a room would
// never close: measured live against dev on 2026-09-13 (411 without the header, 204 with it).
//
// The assertion is on the real FPlayServHttp dispatch — the seam where the header is set —
// not on a fake transport, because the fake is exactly what would hide this.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServRoomsCloseContentLengthTest,
	"PlayServ.Rooms.Close.BodylessPostCarriesContentLength",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServRoomsCloseContentLengthTest::RunTest(const FString& Parameters)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!TestNotNull(TEXT("subsystem"), PS))
	{
		return false;
	}
	UPlayServRooms* Server = PS->GetRooms();

	FFakeClock Clock;
	FPlayServFakeUplinkFactory Factory;
	FPlayServRoomsTestAccess::BeginWithUplink(Server, Factory.Make(), Clock.Fn(), TEXT("blob-arena"));
	TSharedPtr<FPlayServFakeUplinkTransport> Socket = Factory.Current();
	Socket->SimulateConnected();
	Socket->SimulateMessage(MakeHelloAck(TEXT("push"), DefaultRoomConfigJson()));
	FPlayServRoomsTestAccess::AddRegisteredRoom(Server, MakeSnapshot(TEXT("close-content-length")), Clock.Now);

	// The one bodyless write in the SDK. It leaves the process; only its headers are asserted. The
	// room name is this test's alone: the answer removes the room by name whenever it lands.
	Server->CloseRoom(TEXT("close-content-length"), FPlayServSimpleCallback());

	const FPlayServHttp::FLastRequestForTest& Last = FPlayServHttp::GetLastRequestForTest();
	TestEqual(TEXT("the close went out as a POST"), Last.Verb, FString(TEXT("POST")));
	TestTrue(TEXT("on the :close path"), Last.Path.EndsWith(TEXT(":close")));
	TestTrue(TEXT("carrying Content-Length — without it the platform answers 411 and the room never closes"),
		Last.bHasContentLength);

	FPlayServRoomsTestAccess::End(Server);
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Rooms.Browse.SlugLessOverloadsUseTheSetting  (PSV-2645)
//
// Browse(Filters, Cb) and JoinRoom(RoomName, Player, Cb) take the room type from
// UPlayServSettings::RoomDefaultSlug. With one configured they must get PAST the slug check (the
// process is not signed in, so the next refusal is the session one); with none configured they
// must fail fast and say which setting is missing — never a silent empty browse, which is what a
// slug of "" would have produced.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServRoomsSlugLessOverloadsTest,
	"PlayServ.Rooms.Browse.SlugLessOverloadsUseTheSetting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServRoomsSlugLessOverloadsTest::RunTest(const FString& Parameters)
{
	UPlayServRooms* Server = NewObject<UPlayServRooms>();
	Server->AddToRoot();

	UPlayServSettings* Settings = GetMutableDefault<UPlayServSettings>();
	const FString Saved = Settings->RoomDefaultSlug;

	bool bBrowsed = false;
	FPlayServError BrowseError;
	FPlayServBrowsePage BrowsePage;
	const FPlayServBrowseCallback OnBrowse = FPlayServBrowseCallback::CreateLambda(
		[&bBrowsed, &BrowseError, &BrowsePage](bool bOk, const FPlayServBrowsePage& Page, const FPlayServError& Error)
		{
			bBrowsed = bOk;
			BrowsePage = Page;
			BrowseError = Error;
		});

	bool bJoined = false;
	FPlayServError JoinError;
	const FPlayServJoinCallback OnJoin = FPlayServJoinCallback::CreateLambda(
		[&bJoined, &JoinError](bool bOk, const FPlayServJoinResult&, const FPlayServError& Error)
		{
			bJoined = bOk;
			JoinError = Error;
		});

	Settings->RoomDefaultSlug = TEXT("blob-arena");
	Server->Browse(FPlayServRoomFilters(), OnBrowse);
	TestFalse(TEXT("a configured slug is not itself a refusal reason on browse"),
		BrowseError.Message.Contains(TEXT("RoomDefaultSlug is not configured")));
	Server->JoinRoom(TEXT("some-room"), nullptr, OnJoin);
	TestFalse(TEXT("a configured slug is not itself a refusal reason on join"),
		JoinError.Message.Contains(TEXT("RoomDefaultSlug is not configured")));

	Settings->RoomDefaultSlug.Empty();
	Server->Browse(FPlayServRoomFilters(), OnBrowse);
	TestFalse(TEXT("browse without a default slug fails"), bBrowsed);
	TestEqual(TEXT("browse names the missing setting"), BrowseError.Code, EPlayServErrorCode::Unknown);
	TestTrue(TEXT("browse says which setting is missing"), BrowseError.Message.Contains(TEXT("RoomDefaultSlug is not configured")));
	TestEqual(TEXT("browse returns no page rather than an empty one"), BrowsePage.Rooms.Num(), 0);

	Server->JoinRoom(TEXT("some-room"), nullptr, OnJoin);
	TestFalse(TEXT("join without a default slug fails"), bJoined);
	TestEqual(TEXT("join names the missing setting"), JoinError.Code, EPlayServErrorCode::Unknown);
	TestTrue(TEXT("join says which setting is missing"), JoinError.Message.Contains(TEXT("RoomDefaultSlug is not configured")));

	Settings->RoomDefaultSlug = Saved;
	Server->RemoveFromRoot();
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Rooms.Connect.MustBeDeclaredByTheGame  (PSV-2670)
//
// The room's address is the game's to declare and the SDK's to carry unchanged. There is no
// resolution chain behind it any more — no RoomConnectHost, no PLAYSERV_PUBLIC_IP /
// ARBITRIUM_PUBLIC_IP, no local-adapter guess — so a snapshot that names no host is refused
// `connect_not_declared` at registration instead of silently advertising an address the game
// never chose. Driven through the real StartRoom on a Ready uplink, because that refusal and
// the verbatim carry are both decided there.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServRoomsConnectMustBeDeclaredTest,
	"PlayServ.Rooms.Connect.MustBeDeclaredByTheGame",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServRoomsConnectMustBeDeclaredTest::RunTest(const FString& Parameters)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!TestNotNull(TEXT("subsystem"), PS))
	{
		return false;
	}
	UPlayServRooms* Server = PS->GetRooms();
	if (!TestNotNull(TEXT("rooms module"), Server))
	{
		return false;
	}

	FFakeClock Clock;
	FPlayServFakeUplinkFactory Factory;
	FPlayServRoomsTestAccess::BeginWithUplink(Server, Factory.Make(), Clock.Fn(), TEXT("blob-arena"));
	TSharedPtr<FPlayServFakeUplinkTransport> Socket = Factory.Current();
	Socket->SimulateConnected();
	Socket->SimulateMessage(MakeHelloAck(TEXT("push"), DefaultRoomConfigJson()));
	TestTrue(TEXT("uplink Ready with a room configuration"), Server->GetUplinkState() == EPlayServUplinkState::Ready && Server->HasRoomConfig());

	// A port with no host is not an address (IsSet() is host AND port). Nothing resolves it.
	FPlayServRoomSnapshot PortOnly = MakeSnapshot(TEXT("port-only"));
	PortOnly.Connect.Host.Empty();
	PortOnly.Connect.Port = 7781;
	TestFalse(TEXT("a port with no host is not a set connect address"), PortOnly.Connect.IsSet());

	bool bRefusedFired = false;
	FPlayServError Refusal;
	Server->StartRoom(PortOnly, FPlayServSimpleCallback::CreateLambda([&bRefusedFired, &Refusal](bool bSuccess, const FPlayServError& Error)
	{
		bRefusedFired = true;
		Refusal = Error;
		return;
	}));
	TestTrue(TEXT("the refusal is immediate, not a pending registration"), bRefusedFired);
	TestEqual(TEXT("refused ValidationFailed"), Refusal.Code, EPlayServErrorCode::ValidationFailed);
	TestTrue(TEXT("the message names connect_not_declared"), Refusal.Message.Contains(TEXT("connect_not_declared")));
	TestFalse(TEXT("no room was registered"), Server->GetRoomNames().Contains(TEXT("port-only")));

	// A declared address is carried verbatim — the port the game reserved, not a settings default.
	FPlayServRoomSnapshot Declared = MakeSnapshot(TEXT("declared-address"));
	Declared.Connect.Host = TEXT("198.51.100.20");
	Declared.Connect.Port = 7781;
	Server->StartRoom(Declared, FPlayServSimpleCallback());
	TestTrue(TEXT("the room registered"), Server->GetRoomNames().Contains(TEXT("declared-address")));
	const FPlayServRoomConnect Carried = FPlayServRoomsTestAccess::RegisteredConnect(Server, TEXT("declared-address"));
	TestEqual(TEXT("the declared host is carried unchanged"), Carried.Host, FString(TEXT("198.51.100.20")));
	TestEqual(TEXT("the declared port is carried unchanged"), Carried.Port, 7781);

	FPlayServRoomsTestAccess::End(Server);
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Rooms.Settings.DedicatedServerResolution  (PSV-2712)
//
// REGRESSION for PlayServ hosting. A process the platform's launcher started carries its own
// deployment token, and the platform accepts the room it launched only from the instance id that
// token names — its `deployment_id`, a `dpp_…` child record (MatchmakingService waits for exactly
// that id; `contracts/presentation/uplink.md` §1.3). The SDK presented Edgegap's request id or a
// fresh GUID instead, so on a pool machine the room registered under a GUID, nothing answered the
// player who asked for it, and they were refused `room_unreachable` after 90 s.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServRoomsSettingsResolutionTest,
	"PlayServ.Rooms.Settings.DedicatedServerResolution",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServRoomsSettingsResolutionTest::RunTest(const FString& Parameters)
{
	// The room type slug is no longer resolved here: with the RoomDefaultSlug rename (PSV-2645) it
	// became an ordinary command line > PLAYSERV_EXECUTOR_SLUG > ini value on UPlayServSettings,
	// resolved by the same ResolveServerKey helper every other setting uses and covered by
	// PlayServ.Core.Settings.ServerKeyPrecedence.
	TestEqual(TEXT("credential: deployment token wins over the server key"), FPlayServRoomsTestAccess::ResolveCredential(TEXT("eyJ.a.b"), TEXT("sk_x")), FString(TEXT("eyJ.a.b")));
	TestEqual(TEXT("credential: server key when no token"), FPlayServRoomsTestAccess::ResolveCredential(TEXT(""), TEXT("sk_x")), FString(TEXT("sk_x")));

	const FString Issued = TEXT("dpp_0123456789abcdef0123456789abcdef");
	const FString Token = MakeDeploymentToken(FString::Printf(TEXT("{\"sub\":\"executor/prj_1:dev:blob-arena\",\"deployment_id\":\"%s\",\"room_name\":\"r-0123456789abcdef\"}"), *Issued));
	TestEqual(TEXT("instance id: the deployment_id the platform issued, not a minted GUID"), FPlayServRoomsTestAccess::ResolveInstanceId(Token), Issued);

	// Everything without a usable claim gets a fresh GUID — no orchestrator's own variables are read.
	auto IsMinted = [](const FString& Id) { return PlayServRoomsValidation::IsValidInstanceId(Id) && Id.Len() == 36; };
	TestTrue(TEXT("no token: a minted GUID inside the grammar"), IsMinted(FPlayServRoomsTestAccess::ResolveInstanceId(TEXT(""))));
	TestTrue(TEXT("a token without the claim: a minted GUID"), IsMinted(FPlayServRoomsTestAccess::ResolveInstanceId(MakeDeploymentToken(TEXT("{\"sub\":\"executor/x\"}")))));
	TestTrue(TEXT("a claim that is not a string is a broken token, not an id"), IsMinted(FPlayServRoomsTestAccess::ResolveInstanceId(MakeDeploymentToken(TEXT("{\"deployment_id\":12345}")))));
	TestTrue(TEXT("a claim outside the instance-id grammar is not presented"), IsMinted(FPlayServRoomsTestAccess::ResolveInstanceId(MakeDeploymentToken(TEXT("{\"deployment_id\":\"dpp with spaces\"}")))));
	TestTrue(TEXT("a server key is not a token"), IsMinted(FPlayServRoomsTestAccess::ResolveInstanceId(TEXT("sk_dev_abc"))));
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Rooms.Launch.SnapshotFromLaunch  (PSV-2713)
//
// A hosted server registers the room the platform asked for, exactly as the launch describes it:
// the minted name, the machine's public address with the port the launcher allotted, and the
// requesting player's attributes. It derives nothing else — and a launch that names no port or
// no address is refused typed rather than registered under a guess.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServRoomsSnapshotFromLaunchTest,
	"PlayServ.Rooms.Launch.SnapshotFromLaunch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServRoomsSnapshotFromLaunchTest::RunTest(const FString& Parameters)
{
	FPlayServRoomSnapshot Snapshot;
	TestFalse(TEXT("a full launch is a room"), SnapshotOf(FTestLaunch(), Snapshot).IsError());
	TestEqual(TEXT("the minted name, verbatim"), Snapshot.RoomName, FString(TEXT("r-0123456789abcdef")));
	TestEqual(TEXT("connect host is the machine's public address"), Snapshot.Connect.Host, FString(TEXT("203.0.113.40")));
	TestEqual(TEXT("connect port is the allotted listen port"), Snapshot.Connect.Port, 7777);
	TestTrue(TEXT("an Unreal server speaks UDP"), Snapshot.Connect.Transport == EPlayServRoomTransport::Udp);
	TestEqual(TEXT("the requester's map is carried"), Snapshot.Attributes.FindRef(TEXT("map")), FString(TEXT("BlobArenaMap")));
	TestEqual(TEXT("the requester's name is carried"), Snapshot.Attributes.FindRef(TEXT("name")), FString(TEXT("Room of player one")));
	TestEqual(TEXT("capacity is left to the room configuration"), Snapshot.Capacity, 0);

	FPlayServRoomSnapshot Unused;
	const FPlayServError NotLaunched = FPlayServRoomsTestAccess::SnapshotFromLaunch(FString(), 0, FString(), TMap<FString, FString>(), Unused);
	TestEqual(TEXT("no launch: refused ValidationFailed"), NotLaunched.Code, EPlayServErrorCode::ValidationFailed);
	TestTrue(TEXT("no launch: named not_a_platform_launch"), NotLaunched.Message.Contains(TEXT("not_a_platform_launch")));

	FTestLaunch NoPort;
	NoPort.ListenPort = 0;
	TestTrue(TEXT("no port: launch_incomplete"), SnapshotOf(NoPort, Unused).Message.Contains(TEXT("launch_incomplete")));

	FTestLaunch NoHost;
	NoHost.PublicHost.Empty();
	TestTrue(TEXT("no public address: launch_incomplete"), SnapshotOf(NoHost, Unused).Message.Contains(TEXT("launch_incomplete")));

	// The host's seat count travels as the reserved capacity attribute and becomes the room's own capacity.
	FTestLaunch Sized;
	Sized.Attributes.Add(PlayServ::Rooms::Attributes::Capacity, TEXT("6"));
	FPlayServRoomSnapshot SizedRoom;
	TestFalse(TEXT("a launch with the reserved capacity is a room"), SnapshotOf(Sized, SizedRoom).IsError());
	TestEqual(TEXT("the reserved capacity attribute becomes the room's capacity"), SizedRoom.Capacity, 6);
	TestEqual(TEXT("and stays readable as an attribute"), SizedRoom.Attributes.FindRef(PlayServ::Rooms::Attributes::Capacity), FString(TEXT("6")));
	for (const TCHAR* Unusable : { TEXT("0"), TEXT("1001"), TEXT("six"), TEXT("-3"), TEXT("") })
	{
		FTestLaunch Wrong;
		Wrong.Attributes.Add(PlayServ::Rooms::Attributes::Capacity, Unusable);
		FPlayServRoomSnapshot WrongRoom;
		TestFalse(FString::Printf(TEXT("capacity '%s' does not stop the room"), Unusable), SnapshotOf(Wrong, WrongRoom).IsError());
		TestEqual(FString::Printf(TEXT("capacity '%s' is not a seat count: the room configuration's applies"), Unusable), WrongRoom.Capacity, 0);
	}
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Rooms.Launch.StartRoomPlayServHostedRegistersTheLaunchedRoom  (PSV-2713)
//
// The one call a hosted server makes. Driven through the real StartRoomPlayServHosted on a Ready
// uplink: the room that registers is the launch's, address and all; outside a launch the call is
// refused at once and nothing registers.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServRoomsStartLaunchedRoomTest,
	"PlayServ.Rooms.Launch.StartRoomPlayServHostedRegistersTheLaunchedRoom",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServRoomsStartLaunchedRoomTest::RunTest(const FString& Parameters)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!TestNotNull(TEXT("subsystem"), PS))
	{
		return false;
	}
	UPlayServRooms* Server = PS->GetRooms();

	FFakeClock Clock;
	FPlayServFakeUplinkFactory Factory;
	FPlayServRoomsTestAccess::BeginWithUplink(Server, Factory.Make(), Clock.Fn(), TEXT("blob-arena"));
	TSharedPtr<FPlayServFakeUplinkTransport> Socket = Factory.Current();
	Socket->SimulateConnected();
	Socket->SimulateMessage(MakeHelloAck(TEXT("push"), DefaultRoomConfigJson()));

	FPlayServRoomsTestAccess::SetLaunch(Server, FString(), 0, FString(), TMap<FString, FString>());
	bool bRefusedFired = false;
	FPlayServError Refusal;
	Server->StartRoomPlayServHosted(FPlayServSimpleCallback::CreateLambda([&bRefusedFired, &Refusal](bool bSuccess, const FPlayServError& Error)
	{
		bRefusedFired = !bSuccess;
		Refusal = Error;
	}));
	TestTrue(TEXT("outside a launch the refusal is immediate"), bRefusedFired);
	TestTrue(TEXT("and names not_a_platform_launch"), Refusal.Message.Contains(TEXT("not_a_platform_launch")));
	TestEqual(TEXT("and nothing registered"), Server->GetRoomNames().Num(), 0);

	const FTestLaunch Launch;
	FPlayServRoomsTestAccess::SetLaunch(Server, Launch.RoomName, Launch.ListenPort, Launch.PublicHost, Launch.Attributes);
	TestEqual(TEXT("the launched room name reads back through the existing query"), Server->GetLaunchRoomName(), Launch.RoomName);
	Server->StartRoomPlayServHosted(FPlayServSimpleCallback());
	TestTrue(TEXT("the launched room is the one registering"), Server->GetRoomNames().Contains(TEXT("r-0123456789abcdef")));
	const FPlayServRoomConnect Carried = FPlayServRoomsTestAccess::RegisteredConnect(Server, TEXT("r-0123456789abcdef"));
	TestEqual(TEXT("at the machine's public address"), Carried.Host, FString(TEXT("203.0.113.40")));
	TestEqual(TEXT("on the allotted port"), Carried.Port, 7777);

	FPlayServRoomsTestAccess::End(Server);
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Rooms.Server.GetRoomReadsTheRegisteredRoom  (PSV-2707)
//
// A game mode reads its room back: the host's choices are the room's attributes, the capacity is
// the one it registered with. Hosting belongs to the process, so the room outlives a map change —
// and starting it a second time is refused, which is why the new map's game mode asks GetRoom first.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServRoomsGetRoomTest,
	"PlayServ.Rooms.Server.GetRoomReadsTheRegisteredRoom",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServRoomsGetRoomTest::RunTest(const FString& Parameters)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!TestNotNull(TEXT("subsystem"), PS))
	{
		return false;
	}
	UPlayServRooms* Server = PS->GetRooms();

	FFakeClock Clock;
	FPlayServFakeUplinkFactory Factory;
	FPlayServRoomsTestAccess::BeginWithUplink(Server, Factory.Make(), Clock.Fn(), TEXT("blob-arena"));
	TSharedPtr<FPlayServFakeUplinkTransport> Socket = Factory.Current();
	Socket->SimulateConnected();
	Socket->SimulateMessage(MakeHelloAck(TEXT("push"), DefaultRoomConfigJson()));

	FPlayServRoomSnapshot Unknown;
	TestFalse(TEXT("no such room: false"), Server->GetRoom(TEXT("r-0123456789abcdef"), Unknown));

	FTestLaunch Launch;
	Launch.Attributes.Add(PlayServ::Rooms::Attributes::Capacity, TEXT("6"));
	Launch.Attributes.Add(TEXT("tracks"), TEXT("tokyo;monza;spa"));
	FPlayServRoomsTestAccess::SetLaunch(Server, Launch.RoomName, Launch.ListenPort, Launch.PublicHost, Launch.Attributes);
	Server->StartRoomPlayServHosted(FPlayServSimpleCallback());

	FPlayServRoomSnapshot Room;
	TestTrue(TEXT("the registered room reads back"), Server->GetRoom(Launch.RoomName, Room));
	TestEqual(TEXT("its name"), Room.RoomName, Launch.RoomName);
	TestEqual(TEXT("the host's choices are the room's attributes"), Room.Attributes.FindRef(TEXT("tracks")), FString(TEXT("tokyo;monza;spa")));
	TestEqual(TEXT("the capacity it registered with"), Room.Capacity, 6);
	TestEqual(TEXT("its address"), Room.Connect.Port, 7777);

	bool bSecondRefused = false;
	Server->StartRoomPlayServHosted(FPlayServSimpleCallback::CreateLambda([&bSecondRefused](bool bSuccess, const FPlayServError&) { bSecondRefused = !bSuccess; }));
	TestTrue(TEXT("starting the same room again is refused — a new map's game mode checks GetRoom first"), bSecondRefused);

	Room.State = TEXT("race 2/3");
	TestTrue(TEXT("the room read back is updated in place"), Server->UpdateRoom(Room));
	FPlayServRoomSnapshot Updated;
	Server->GetRoom(Launch.RoomName, Updated);
	TestEqual(TEXT("and reads back updated"), Updated.State, FString(TEXT("race 2/3")));

	FPlayServRoomsTestAccess::End(Server);
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Rooms.Launch.AttributesAreReadVerbatim  (PSV-2712)
//
// PLAYSERV_ROOM_ATTRIBUTES is the requester's JSON object, verbatim. It is read with the same rule
// browse uses — scalars as their string form, containers as compact JSON — and text that is not an
// object carries no attributes rather than half of them.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServRoomsLaunchAttributesTest,
	"PlayServ.Rooms.Launch.AttributesAreReadVerbatim",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServRoomsLaunchAttributesTest::RunTest(const FString& Parameters)
{
	TMap<FString, FString> Attributes;
	TestTrue(TEXT("an object parses"), PlayServRoomsClientWire::ParseAttributesJson(TEXT("{\"map\":\"BlobArenaMap\",\"laps\":3,\"ranked\":true,\"mods\":[\"a\"]}"), Attributes));
	TestEqual(TEXT("a string stays a string"), Attributes.FindRef(TEXT("map")), FString(TEXT("BlobArenaMap")));
	TestEqual(TEXT("a number takes its string form"), Attributes.FindRef(TEXT("laps")), FString(TEXT("3")));
	TestEqual(TEXT("a bool takes its string form"), Attributes.FindRef(TEXT("ranked")), FString(TEXT("true")));
	TestEqual(TEXT("a container is compact JSON"), Attributes.FindRef(TEXT("mods")), FString(TEXT("[\"a\"]")));

	TMap<FString, FString> None;
	TestFalse(TEXT("text that is not an object is refused"), PlayServRoomsClientWire::ParseAttributesJson(TEXT("[1,2]"), None));
	TestFalse(TEXT("empty text is refused"), PlayServRoomsClientWire::ParseAttributesJson(TEXT(""), None));
	TestEqual(TEXT("and adds nothing"), None.Num(), 0);
	TestTrue(TEXT("the platform's empty object is no attributes"), PlayServRoomsClientWire::ParseAttributesJson(TEXT("{}"), None) && None.Num() == 0);
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Rooms.Client.HostBodyAndReadyRoom  (PSV-2693)
//
// `:host` carries only the requester's attributes — the platform mints the room's name — and its
// answer is read as the ready room. The answer's `rsv_*` reservation seats nobody and never reaches
// the game: FPlayServRoomListing has no field for it, so a game cannot travel on it by accident.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServRoomsHostWireTest,
	"PlayServ.Rooms.Client.HostBodyAndReadyRoom",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServRoomsHostWireTest::RunTest(const FString& Parameters)
{
	TMap<FString, FString> Attributes;
	Attributes.Add(TEXT("map"), TEXT("BlobArenaMap"));
	const TSharedPtr<FJsonObject> Body = PlayServRoomsClientWire::BuildHostBody(Attributes);
	const TSharedPtr<FJsonObject>* Sent = nullptr;
	TestTrue(TEXT("attributes ride as an object"), Body->TryGetObjectField(TEXT("attributes"), Sent));
	TestEqual(TEXT("verbatim"), Sent != nullptr ? (*Sent)->GetStringField(TEXT("map")) : FString(), FString(TEXT("BlobArenaMap")));
	TestFalse(TEXT("no room_name — the platform refuses one"), Body->HasField(TEXT("room_name")));
	TestEqual(TEXT("no attributes is an empty body"), PlayServRoomsClientWire::BuildHostBody(TMap<FString, FString>())->Values.Num(), 0);

	const TSharedPtr<FJsonObject> Answer = FPlayServFakeUplinkTransport::ParseJson(TEXT("{\"status\":\"matched\",\"room_name\":\"r-0123456789abcdef\",\"reservation_token\":\"rsv_secret\",\"expires_in\":19,\"connect\":{\"host\":\"203.0.113.40\",\"port\":7777,\"transport\":\"udp\"},\"region\":\"fra\",\"attributes\":{\"map\":\"BlobArenaMap\"}}"));
	FPlayServRoomListing Room;
	TestTrue(TEXT("the answer is a room"), PlayServRoomsClientWire::ParseHostedRoom(Answer, Room));
	TestEqual(TEXT("its minted name"), Room.RoomName, FString(TEXT("r-0123456789abcdef")));
	TestEqual(TEXT("the address its server registered"), Room.Connect.Host, FString(TEXT("203.0.113.40")));
	TestEqual(TEXT("and port"), Room.Connect.Port, 7777);
	TestEqual(TEXT("its region"), Room.Region, FString(TEXT("fra")));
	TestEqual(TEXT("the attributes as registered"), Room.Attributes.FindRef(TEXT("map")), FString(TEXT("BlobArenaMap")));
	TestEqual(TEXT("empty: the platform seats nobody on a host"), Room.Players, 0);

	FPlayServRoomListing Nameless;
	TestFalse(TEXT("an answer that names no room is not a room"), PlayServRoomsClientWire::ParseHostedRoom(FPlayServFakeUplinkTransport::ParseJson(TEXT("{\"status\":\"matched\"}")), Nameless));
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Rooms.Data.SubscribeResendsAndRoutesUpdates
//
// SubscribeData speaks the C# SDK's RuntimeData wire: `subscribe_data` names the entity and its key path, and the
// platform answers with a `data_update` per changed record, upserts and deletes alike. A subscription lives on the
// socket it went out on, so every new socket carries it again, and the game hears each time that it went out: what
// changed in between is not sent again.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServRoomsDataSubscriptionTest,
	"PlayServ.Rooms.Data.SubscribeResendsAndRoutesUpdates",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServRoomsDataSubscriptionTest::RunTest(const FString& Parameters)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!TestNotNull(TEXT("subsystem"), PS))
	{
		return false;
	}
	UPlayServRooms* Server = PS->GetRooms();

	FFakeClock Clock;
	FPlayServFakeUplinkFactory Factory;
	FPlayServRoomsTestAccess::BeginWithUplink(Server, Factory.Make(), Clock.Fn(), TEXT("blob-arena"));
	TArray<FString> Subscribed;
	TArray<FPlayServDataUpdate> Updates;
	const FDelegateHandle SubscribedHandle = Server->OnDataSubscribed.AddLambda([&Subscribed](const FString& Entity) { Subscribed.Add(Entity); });
	const FDelegateHandle UpdateHandle = Server->OnDataUpdate.AddLambda([&Updates](const FPlayServDataUpdate& Update) { Updates.Add(Update); });

	TSharedPtr<FPlayServFakeUplinkTransport> First = Factory.Current();
	First->SimulateConnected();
	Server->SubscribeData(TEXT("WorldCube"), TEXT("field:key"));
	TestEqual(TEXT("nothing goes out before the hello ack"), First->CountSentOfType(TEXT("subscribe_data")), 0);
	TestEqual(TEXT("and nothing is reported"), Subscribed.Num(), 0);

	First->SimulateMessage(MakeHelloAck(TEXT("push"), DefaultRoomConfigJson()));
	const TSharedPtr<FJsonObject> Frame = First->LastSentOfType(TEXT("subscribe_data"));
	if (TestNotNull(TEXT("the subscription goes out once the uplink is ready"), Frame.Get()))
	{
		TestEqual(TEXT("entity"), Frame->GetStringField(TEXT("entity")), FString(TEXT("WorldCube")));
		TestEqual(TEXT("key path"), Frame->GetStringField(TEXT("key_path")), FString(TEXT("field:key")));
		TestTrue(TEXT("project_id and client_key are empty, as the C# SDK sends them"),
			Frame->HasField(TEXT("project_id")) && Frame->GetStringField(TEXT("project_id")).IsEmpty()
			&& Frame->HasField(TEXT("client_key")) && Frame->GetStringField(TEXT("client_key")).IsEmpty());
	}
	TestEqual(TEXT("sent once"), First->CountSentOfType(TEXT("subscribe_data")), 1);
	TestTrue(TEXT("and reported"), Subscribed.Num() == 1 && Subscribed[0] == TEXT("WorldCube"));

	// A delete a cloud function made over HTTP, an upsert another server wrote, and a delete with no record.
	First->SimulateMessage(TEXT("{\"type\":\"data_update\",\"entity\":\"WorldCube\",\"id\":\"rec_1\",\"op\":\"delete\",\"data\":{\"key\":\"5:6:-1\",\"x\":5,\"y\":6,\"z\":-1,\"kind\":\"air\"}}"));
	First->SimulateMessage(TEXT("{\"type\":\"data_update\",\"entity\":\"WorldCube\",\"id\":\"rec_2\",\"op\":\"upsert\",\"data\":{\"key\":\"1:2:0\",\"x\":1,\"y\":2,\"z\":0,\"kind\":\"brick\"}}"));
	First->SimulateMessage(TEXT("{\"type\":\"data_update\",\"entity\":\"WorldCube\",\"id\":\"rec_3\",\"op\":\"delete\"}"));
	if (TestEqual(TEXT("every update reaches the game"), Updates.Num(), 3))
	{
		TestTrue(TEXT("a delete is a delete"), Updates[0].IsDelete() && Updates[0].Entity == TEXT("WorldCube") && Updates[0].Id == TEXT("rec_1"));
		TestTrue(TEXT("with the record it removed"), Updates[0].Data.IsValid() && Updates[0].Data->GetNumberField(TEXT("z")) == -1.0);
		TestTrue(TEXT("an upsert is not"), !Updates[1].IsDelete() && Updates[1].Data.IsValid() && Updates[1].Data->GetStringField(TEXT("kind")) == TEXT("brick"));
		TestTrue(TEXT("an update without data still arrives, with none"), Updates[2].IsDelete() && !Updates[2].Data.IsValid());
	}
	First->SimulateMessage(TEXT("{\"type\":\"data_update\",\"id\":\"rec_4\",\"op\":\"delete\"}"));
	TestEqual(TEXT("an update that names no entity is dropped"), Updates.Num(), 3);

	// A renewed session token on the same socket leaves the subscription where it is.
	First->SimulateMessage(TEXT("{\"type\":\"session_token\",\"session_token\":\"eyJ.renewed.token\",\"expires_in\":3600}"));
	TestEqual(TEXT("a renewed token sends nothing again"), First->CountSentOfType(TEXT("subscribe_data")), 1);

	// A new socket carries every subscription again, and the game hears that it went out.
	First->SimulateClosed(1012, TEXT("Service Restart"));
	Clock.Now += 2.0;
	FPlayServRoomsTestAccess::Tick(Server, Clock.Now);
	TSharedPtr<FPlayServFakeUplinkTransport> Second = Factory.Current();
	TestTrue(TEXT("the client reconnected on a second socket"), Second.IsValid() && Second != First);
	Second->SimulateConnected();
	Second->SimulateMessage(MakeHelloAck(TEXT("push"), DefaultRoomConfigJson()));
	TestEqual(TEXT("the new socket subscribes again"), Second->CountSentOfType(TEXT("subscribe_data")), 1);
	TestEqual(TEXT("and it is reported again"), Subscribed.Num(), 2);

	// On a ready uplink a subscription goes out at once; an entity unsubscribed stays off the next socket.
	Server->SubscribeData(TEXT("WorldBomb"), TEXT("field:bomb_id"));
	TestEqual(TEXT("at once on a ready uplink"), Second->CountSentOfType(TEXT("subscribe_data")), 2);
	Server->UnsubscribeData(TEXT("WorldCube"));
	const TSharedPtr<FJsonObject> Off = Second->LastSentOfType(TEXT("unsubscribe_data"));
	TestTrue(TEXT("unsubscribe names the entity"), Off.IsValid() && Off->GetStringField(TEXT("entity")) == TEXT("WorldCube"));
	Second->SimulateClosed(1012, TEXT("Service Restart"));
	Clock.Now += 2.0;
	FPlayServRoomsTestAccess::Tick(Server, Clock.Now);
	TSharedPtr<FPlayServFakeUplinkTransport> Third = Factory.Current();
	Third->SimulateConnected();
	Third->SimulateMessage(MakeHelloAck(TEXT("push"), DefaultRoomConfigJson()));
	const TSharedPtr<FJsonObject> Again = Third->LastSentOfType(TEXT("subscribe_data"));
	TestTrue(TEXT("only the entity still subscribed goes out"),
		Third->CountSentOfType(TEXT("subscribe_data")) == 1 && Again.IsValid() && Again->GetStringField(TEXT("entity")) == TEXT("WorldBomb"));

	Server->OnDataSubscribed.Remove(SubscribedHandle);
	Server->OnDataUpdate.Remove(UpdateHandle);
	FPlayServRoomsTestAccess::End(Server);
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Rooms.Data.WritesGoOutAsDataWriteFrames
//
// WriteData and DeleteData speak the C# SDK's RuntimeData wire: a `data_write` frame names the entity, the op, the
// record's business key and its fields; the platform upserts or deletes the row by that key. project_id is empty, as for
// a subscription: the platform takes the uplink's own project. A delete carries an empty data object, the shape the
// platform's contract shows (the C# SDK's delete sent none and never left the server, PSV-2989). Nothing is queued:
// while the uplink is not ready a write is refused.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServRoomsDataWriteTest,
	"PlayServ.Rooms.Data.WritesGoOutAsDataWriteFrames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServRoomsDataWriteTest::RunTest(const FString& Parameters)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!TestNotNull(TEXT("subsystem"), PS))
	{
		return false;
	}
	UPlayServRooms* Server = PS->GetRooms();

	FFakeClock Clock;
	FPlayServFakeUplinkFactory Factory;
	FPlayServRoomsTestAccess::BeginWithUplink(Server, Factory.Make(), Clock.Fn(), TEXT("blob-arena"));
	TSharedPtr<FPlayServFakeUplinkTransport> Socket = Factory.Current();
	Socket->SimulateConnected();

	TSharedRef<FJsonObject> Pose = MakeShared<FJsonObject>();
	Pose->SetStringField(TEXT("player_id"), TEXT("plr_1"));
	Pose->SetNumberField(TEXT("x"), 23.5);
	Pose->SetNumberField(TEXT("sneaking"), 0);
	TestFalse(TEXT("before the hello ack a write is refused"), Server->WriteData(TEXT("WorldPresence"), TEXT("plr_1"), Pose));
	TestEqual(TEXT("and nothing goes out, nor later"), Socket->CountSentOfType(TEXT("data_write")), 0);

	Socket->SimulateMessage(MakeHelloAck(TEXT("push"), DefaultRoomConfigJson()));
	TestEqual(TEXT("a refused write is not sent once the uplink is ready"), Socket->CountSentOfType(TEXT("data_write")), 0);
	TestTrue(TEXT("on a ready uplink a write goes out"), Server->WriteData(TEXT("WorldPresence"), TEXT("plr_1"), Pose));
	const TSharedPtr<FJsonObject> Upsert = Socket->LastSentOfType(TEXT("data_write"));
	if (TestNotNull(TEXT("as a data_write frame"), Upsert.Get()))
	{
		TestEqual(TEXT("entity"), Upsert->GetStringField(TEXT("entity")), FString(TEXT("WorldPresence")));
		TestEqual(TEXT("an upsert"), Upsert->GetStringField(TEXT("op")), FString(TEXT("upsert")));
		TestEqual(TEXT("by its business key"), Upsert->GetStringField(TEXT("id")), FString(TEXT("plr_1")));
		TestTrue(TEXT("project_id is there and empty"), Upsert->HasField(TEXT("project_id")) && Upsert->GetStringField(TEXT("project_id")).IsEmpty());
		const TSharedPtr<FJsonObject>* Data = nullptr;
		TestTrue(TEXT("with the record's fields as an object"), Upsert->TryGetObjectField(TEXT("data"), Data)
			&& (*Data)->GetNumberField(TEXT("x")) == 23.5 && (*Data)->GetStringField(TEXT("player_id")) == TEXT("plr_1") && (*Data)->HasField(TEXT("sneaking")));
	}

	TestTrue(TEXT("a delete goes out"), Server->DeleteData(TEXT("WorldPresence"), TEXT("plr_1")));
	const TSharedPtr<FJsonObject> Delete = Socket->LastSentOfType(TEXT("data_write"));
	if (TestNotNull(TEXT("as a data_write frame too"), Delete.Get()))
	{
		TestEqual(TEXT("a delete"), Delete->GetStringField(TEXT("op")), FString(TEXT("delete")));
		TestEqual(TEXT("by the same key"), Delete->GetStringField(TEXT("id")), FString(TEXT("plr_1")));
		const TSharedPtr<FJsonObject>* Data = nullptr;
		TestTrue(TEXT("with an empty data object, not none"), Delete->TryGetObjectField(TEXT("data"), Data) && (*Data)->Values.Num() == 0);
	}
	TestEqual(TEXT("two frames in all"), Socket->CountSentOfType(TEXT("data_write")), 2);

	TestFalse(TEXT("no entity: refused"), Server->WriteData(FString(), TEXT("plr_1"), Pose));
	TestFalse(TEXT("no key: refused"), Server->DeleteData(TEXT("WorldPresence"), FString()));
	TestEqual(TEXT("and not sent"), Socket->CountSentOfType(TEXT("data_write")), 2);

	Socket->SimulateClosed(1012, TEXT("Service Restart"));
	TestFalse(TEXT("a closed uplink refuses a write"), Server->WriteData(TEXT("WorldPresence"), TEXT("plr_1"), Pose));

	FPlayServRoomsTestAccess::End(Server);
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Rooms.Logs.LinesWaitForTheUplinkAndGoOutAsLogFrames
//
// Log speaks the C# SDK's Platform.Log over the uplink: a `log` frame with message, level and optional data. On a
// pool server a line logged before the uplink is ready waits for it (the C# SDK's dial-in buffer of 200): the platform
// files it under the game server's function either way.
// ---------------------------------------------------------------------------

namespace
{
	TArray<TSharedPtr<FJsonObject>> SentLogs(const FPlayServFakeUplinkTransport& Transport)
	{
		TArray<TSharedPtr<FJsonObject>> Logs;
		for (int32 I = 0; I < Transport.Sent.Num(); ++I)
		{
			const TSharedPtr<FJsonObject> Frame = Transport.SentJson(I);
			FString Type;
			if (Frame.IsValid() && Frame->TryGetStringField(TEXT("type"), Type) && Type == TEXT("log")) Logs.Add(Frame);
		}
		return Logs;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServRoomsLogTest,
	"PlayServ.Rooms.Logs.LinesWaitForTheUplinkAndGoOutAsLogFrames",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServRoomsLogTest::RunTest(const FString& Parameters)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!TestNotNull(TEXT("subsystem"), PS))
	{
		return false;
	}
	UPlayServRooms* Server = PS->GetRooms();
	FFakeClock Clock;
	FPlayServFakeUplinkFactory Factory;
	FPlayServRoomsTestAccess::BeginWithUplink(Server, Factory.Make(), Clock.Fn(), TEXT("blob-arena"));

	TSharedPtr<FPlayServFakeUplinkTransport> First = Factory.Current();
	First->SimulateConnected();
	Server->Log(TEXT("world loaded"), EPlayServLogLevel::Info);
	Server->Log(TEXT("region 3 is held"), EPlayServLogLevel::Warn);
	TestEqual(TEXT("nothing goes out before the hello ack"), SentLogs(*First).Num(), 0);

	First->SimulateMessage(MakeHelloAck(TEXT("push"), DefaultRoomConfigJson()));
	TArray<TSharedPtr<FJsonObject>> Logs = SentLogs(*First);
	if (TestEqual(TEXT("the waiting lines go out once the uplink is ready"), Logs.Num(), 2))
	{
		TestEqual(TEXT("oldest first"), Logs[0]->GetStringField(TEXT("message")), FString(TEXT("world loaded")));
		TestEqual(TEXT("info is the C# SDK's info"), Logs[0]->GetStringField(TEXT("level")), FString(TEXT("info")));
		TestEqual(TEXT("then the warning"), Logs[1]->GetStringField(TEXT("message")), FString(TEXT("region 3 is held")));
		TestEqual(TEXT("as warn"), Logs[1]->GetStringField(TEXT("level")), FString(TEXT("warn")));
		TestFalse(TEXT("a line without data carries none"), Logs[0]->HasField(TEXT("data")));
	}

	// On a ready uplink a line goes at once, with its data.
	TSharedPtr<FJsonObject> Data = MakeShared<FJsonObject>();
	Data->SetNumberField(TEXT("players"), 3);
	Server->Log(TEXT("room closed"), EPlayServLogLevel::Error, Data);
	Logs = SentLogs(*First);
	if (TestEqual(TEXT("at once on a ready uplink"), Logs.Num(), 3))
	{
		TestEqual(TEXT("error"), Logs[2]->GetStringField(TEXT("level")), FString(TEXT("error")));
		const TSharedPtr<FJsonObject>* Carried = nullptr;
		TestTrue(TEXT("with its data"), Logs[2]->TryGetObjectField(TEXT("data"), Carried) && (*Carried)->GetNumberField(TEXT("players")) == 3.0);
	}
	Server->Log(FString::ChrN(UPlayServRooms::MaxLogMessageChars + 500, TEXT('x')), EPlayServLogLevel::Debug);
	TestEqual(TEXT("a very long line is cut"), SentLogs(*First).Last()->GetStringField(TEXT("message")).Len(), UPlayServRooms::MaxLogMessageChars + 3);

	// While the uplink is down the last 200 wait, after a line that counts the dropped ones.
	First->SimulateClosed(1012, TEXT("Service Restart"));
	for (int32 I = 0; I < UPlayServRooms::MaxPendingLogLines + 5; ++I) Server->Log(FString::Printf(TEXT("line %d"), I), EPlayServLogLevel::Info);
	Clock.Now += 2.0;
	FPlayServRoomsTestAccess::Tick(Server, Clock.Now);
	TSharedPtr<FPlayServFakeUplinkTransport> Second = Factory.Current();
	Second->SimulateConnected();
	Second->SimulateMessage(MakeHelloAck(TEXT("push"), DefaultRoomConfigJson()));
	Logs = SentLogs(*Second);
	if (TestEqual(TEXT("the notice and the last 200 go out on the next socket"), Logs.Num(), UPlayServRooms::MaxPendingLogLines + 1))
	{
		TestTrue(TEXT("the notice counts the 5 dropped"), Logs[0]->GetStringField(TEXT("message")).Contains(TEXT("5 log line(s) not sent")));
		TestEqual(TEXT("then the oldest kept"), Logs[1]->GetStringField(TEXT("message")), FString(TEXT("line 5")));
		TestEqual(TEXT("and the newest last"), Logs.Last()->GetStringField(TEXT("message")), FString::Printf(TEXT("line %d"), UPlayServRooms::MaxPendingLogLines + 4));
	}

	FPlayServRoomsTestAccess::End(Server);
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Rooms.Logs.ForwardingPicksLinesByCategoryVerbosityAndRate
//
// ForwardLogs catches the process's own UE_LOG lines: a named category from its own verbosity, every other one from the
// rules' Everything, at most so many lines in ten seconds. A line the sending itself logs is not caught again.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServLogForwardingTest,
	"PlayServ.Rooms.Logs.ForwardingPicksLinesByCategoryVerbosityAndRate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServLogForwardingTest::RunTest(const FString& Parameters)
{
	FPlayServLogForwarding Rules;
	Rules.Categories.Add(TEXT("LogGame"), ELogVerbosity::Log);
	Rules.MaxLinesPerTenSeconds = 3;
	EPlayServLogLevel Level;
	TestTrue(TEXT("the game's own line goes"), FPlayServLogForwarder::Wanted(Rules, TEXT("LogGame"), ELogVerbosity::Log, Level) && Level == EPlayServLogLevel::Info);
	TestTrue(TEXT("as does its warning, as warn"), FPlayServLogForwarder::Wanted(Rules, TEXT("LogGame"), ELogVerbosity::Warning, Level) && Level == EPlayServLogLevel::Warn);
	TestFalse(TEXT("not its verbose line"), FPlayServLogForwarder::Wanted(Rules, TEXT("LogGame"), ELogVerbosity::Verbose, Level));
	TestFalse(TEXT("another category's warning stays"), FPlayServLogForwarder::Wanted(Rules, TEXT("LogOther"), ELogVerbosity::Warning, Level));
	TestTrue(TEXT("every category's error goes"), FPlayServLogForwarder::Wanted(Rules, TEXT("LogOther"), ELogVerbosity::Error, Level) && Level == EPlayServLogLevel::Error);
	TestTrue(TEXT("and a fatal one, as error"), FPlayServLogForwarder::Wanted(Rules, TEXT("LogOther"), ELogVerbosity::Fatal, Level) && Level == EPlayServLogLevel::Error);
	TestEqual(TEXT("a room ticket in a travel URL is blanked out"),
		FPlayServLogForwarder::Redacted(TEXT("Join request: /Engine/Maps/Entry?rsv=rsv_0a1b2c?game=/Script/X")), FString(TEXT("Join request: /Engine/Maps/Entry?rsv=<redacted>?game=/Script/X")));
	TestEqual(TEXT("every one, in any case"), FPlayServLogForwarder::Redacted(TEXT("a rsv=one b RSV=two")), FString(TEXT("a rsv=<redacted> b RSV=<redacted>")));
	TestEqual(TEXT("a line without one is left as it is"), FPlayServLogForwarder::Redacted(TEXT("world ready")), FString(TEXT("world ready")));

	double Now = 100.0;
	FPlayServLogForwarder Forwarder(Rules, [&Now]() { return Now; });
	for (int32 I = 0; I < 5; ++I) Forwarder.Serialize(*FString::Printf(TEXT("joined %d"), I), ELogVerbosity::Log, TEXT("LogGame"));
	Forwarder.Serialize(TEXT("ignored"), ELogVerbosity::Log, TEXT("LogOther"));
	TArray<TPair<FString, EPlayServLogLevel>> Out;
	Forwarder.Drain([&Out, &Forwarder](const FString& Text, EPlayServLogLevel L)
	{
		Out.Add({ Text, L });
		// The sending logs about itself: that line is not caught again.
		Forwarder.Serialize(TEXT("sent a line"), ELogVerbosity::Log, TEXT("LogGame"));
	});
	if (TestEqual(TEXT("three lines in ten seconds, and the count of the rest"), Out.Num(), 4))
	{
		TestEqual(TEXT("with the category in front"), Out[0].Key, FString(TEXT("LogGame: joined 0")));
		TestEqual(TEXT("in order"), Out[2].Key, FString(TEXT("LogGame: joined 2")));
		TestTrue(TEXT("the count of the two left out, as a warning"), Out[3].Key.Contains(TEXT("2 log line(s) not sent")) && Out[3].Value == EPlayServLogLevel::Warn);
	}
	Out.Reset();
	Forwarder.Drain([&Out](const FString& Text, EPlayServLogLevel L) { Out.Add({ Text, L }); });
	TestEqual(TEXT("the sending's own line was not caught"), Out.Num(), 0);

	Now += 10.0;
	Forwarder.Serialize(TEXT("ten seconds on"), ELogVerbosity::Display, TEXT("LogGame"));
	Forwarder.Drain([&Out](const FString& Text, EPlayServLogLevel L) { Out.Add({ Text, L }); });
	TestTrue(TEXT("a new ten seconds lets lines through again"), Out.Num() == 1 && Out[0].Key == TEXT("LogGame: ten seconds on"));
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Auth.LoginServerAcceptsDeploymentToken
//
// Decision 14: a platform-started server presents a deployment token (a JWT) instead of sk_*.
// LoginServer must take that shape and still refuse a pk_* client key at the boundary.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthDeploymentTokenTest,
	"PlayServ.Auth.LoginServerAcceptsDeploymentToken",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthDeploymentTokenTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("three base64url segments are JWT-shaped"), FPlayServAuthTestAccess::IsJwtShaped(TEXT("eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiJkZXBsb3ltZW50L3gifQ.c2lnbmF0dXJl")));
	TestFalse(TEXT("an sk_ key is not"), FPlayServAuthTestAccess::IsJwtShaped(TEXT("sk_dev_abc")));
	TestFalse(TEXT("two segments are not"), FPlayServAuthTestAccess::IsJwtShaped(TEXT("a.b")));
	TestFalse(TEXT("an empty segment is not"), FPlayServAuthTestAccess::IsJwtShaped(TEXT("a..c")));
	TestFalse(TEXT("a pk_ key is not"), FPlayServAuthTestAccess::IsJwtShaped(TEXT("pk_dev_abc")));

	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!TestNotNull(TEXT("subsystem"), PS))
	{
		return false;
	}
	UPlayServAuth* Auth = PS->GetAuth();

	bool bAccepted = false;
	Auth->LoginServer(TEXT("eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiJkZXBsb3ltZW50L3gifQ.c2lnbmF0dXJl"), FPlayServSimpleCallback::CreateLambda([&bAccepted](bool bSuccess, const FPlayServError&)
	{
		bAccepted = bSuccess;
	}));
	TestTrue(TEXT("a deployment token opens a server session"), bAccepted && Auth->IsServerSession());

	bool bRefused = false;
	Auth->LoginServer(TEXT("pk_dev_abc"), FPlayServSimpleCallback::CreateLambda([&bRefused](bool bSuccess, const FPlayServError&)
	{
		bRefused = !bSuccess;
	}));
	TestTrue(TEXT("a pk_ key is refused at the boundary"), bRefused);

	Auth->Logout(FPlayServSimpleCallback());
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Rooms.Admission.AdmitVerifiedReportsTheJoinAndTheLeave
//
// A player who comes in through the game's own transport (Cube World's WebSocket door for browsers)
// never reaches the engine's PostLogin. AdmitVerified is their PostLogin: the platform hears the join
// with the reservation token, once; RemovePlayer is their leave. A refused verdict, a ticket never
// verified here and one admitted already are not reported.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServRoomsAdmitVerifiedTest,
	"PlayServ.Rooms.Admission.AdmitVerifiedReportsTheJoinAndTheLeave",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServRoomsAdmitVerifiedTest::RunTest(const FString& Parameters)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	if (!TestNotNull(TEXT("subsystem"), PS))
	{
		return false;
	}
	UPlayServRooms* Server = PS->GetRooms();
	FFakeClock Clock;
	FPlayServFakeUplinkFactory Factory;
	TSharedPtr<FPlayServFakeUplinkTransport> Socket = BeginHostingOneRoom(Server, Factory, Clock, TEXT("blob-7a3f"));

	const int32 FramesBefore = Socket->CountSentOfType(TEXT("room_presence"));
	Socket->SimulateMessage(TEXT("{\"type\":\"ticket_offer\",\"reservation_token\":\"rsv_web\",\"room_name\":\"blob-7a3f\",\"player_id\":\"plr_web\",\"expires_in\":10}"));
	const FPlayServTicketVerdict Verdict = Server->VerifyTicket(TEXT("rsv_web"));
	TestTrue(TEXT("the door's ticket is accepted"), Verdict.bAccepted && Verdict.PlayerId == TEXT("plr_web"));
	TestEqual(TEXT("verifying alone reports nothing (the bug: the door stopped here)"), Socket->CountSentOfType(TEXT("room_presence")), FramesBefore);

	TestTrue(TEXT("AdmitVerified admits the player"), Server->AdmitVerified(Verdict));
	TSharedPtr<FJsonObject> Join = Socket->LastSentOfType(TEXT("room_presence"));
	TestTrue(TEXT("one join frame, with the reservation token"), Socket->CountSentOfType(TEXT("room_presence")) == FramesBefore + 1 && Join.IsValid()
		&& Join->GetStringField(TEXT("event")) == TEXT("join") && Join->GetStringField(TEXT("player_id")) == TEXT("plr_web")
		&& Join->GetStringField(TEXT("reservation_token")) == TEXT("rsv_web"));
	TestTrue(TEXT("the player is in the roster"), FPlayServRoomsTestAccess::RosterContains(Server, TEXT("blob-7a3f"), TEXT("plr_web")));
	TestEqual(TEXT("player count"), Server->GetRoomPlayerCount(TEXT("blob-7a3f")), 1);

	TestFalse(TEXT("the same ticket is not admitted twice"), Server->AdmitVerified(Verdict));
	FPlayServTicketVerdict Refused;
	Refused.ReservationToken = TEXT("rsv_web");
	Refused.PlayerId = TEXT("plr_web");
	TestFalse(TEXT("a refused verdict is not admitted"), Server->AdmitVerified(Refused));
	FPlayServTicketVerdict Never = Verdict;
	Never.ReservationToken = TEXT("rsv_never_verified");
	TestFalse(TEXT("a ticket never verified here is not admitted"), Server->AdmitVerified(Never));
	TestEqual(TEXT("and none of them sends a frame"), Socket->CountSentOfType(TEXT("room_presence")), FramesBefore + 1);

	TestTrue(TEXT("RemovePlayer is the leave"), Server->RemovePlayer(TEXT("blob-7a3f"), TEXT("plr_web")));
	TSharedPtr<FJsonObject> Leave = Socket->LastSentOfType(TEXT("room_presence"));
	TestTrue(TEXT("a leave frame at once"), Leave.IsValid() && Leave->GetStringField(TEXT("event")) == TEXT("leave") && Leave->GetStringField(TEXT("player_id")) == TEXT("plr_web"));
	TestFalse(TEXT("off the roster"), FPlayServRoomsTestAccess::RosterContains(Server, TEXT("blob-7a3f"), TEXT("plr_web")));

	FPlayServRoomsTestAccess::End(Server);
	return true;
}

#endif // !UE_BUILD_SHIPPING
