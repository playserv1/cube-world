#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Core/PlayServTypes.h"
#include "Rooms/PlayServRoomsTypes.h"
#include "PlayServRooms.generated.h"

class AController;
class AGameModeBase;
class APlayerController;
class FJsonObject;
class FPlayServHttp;
class FPlayServUplinkClient;
class FPlayServAdmissionTable;
class UWorld;
struct FPlayServJoinRound;
struct FPlayServRoomRuntime;
struct FPlayServUplinkAck;
namespace PlayServRoomsClientWire { struct FJoinStepDecision; }

/** The uplink changed state (connected, hello sent, ready, or dropped). */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FPlayServOnUplinkStateChanged, EPlayServUplinkState, State);

/** A new room configuration arrived from the platform and is now in effect. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FPlayServOnRoomConfigChanged, const FPlayServRoomConfig&, Config);

/** The platform's placement state for one room changed. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FPlayServOnRoomPlacementChanged, const FString&, RoomName, EPlayServPlacementState, State);

/** The SDK ended a room on its own (lifetime or idle timeout) or lost it to the platform's verdict. Reason names which. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FPlayServOnRoomEnded, const FString&, RoomName, const FString&, Reason);

/** The SDK removed a player from a room's roster; the game should disconnect them. Reason names why. */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FPlayServOnPlayerRemoved, const FString&, RoomName, const FString&, PlayerId, const FString&, Reason);

/** The platform offered a ticket for one of this server's rooms. Return false to refuse it; leave unbound to accept every offer. */
DECLARE_DELEGATE_RetVal_ThreeParams(bool, FPlayServTicketOfferDecision, const FString& /*RoomName*/, const FString& /*PlayerId*/, FString& /*OutRefusalDetail*/);

/** One page of Browse, or the typed refusal. */
DECLARE_DELEGATE_ThreeParams(FPlayServBrowseCallback, bool /*bSuccess*/, const FPlayServBrowsePage& /*Page*/, const FPlayServError& /*Error*/);

/** The end of a join: a ticket, or the platform's typed refusal. */
DECLARE_DELEGATE_ThreeParams(FPlayServJoinCallback, bool /*bSuccess*/, const FPlayServJoinResult& /*Result*/, const FPlayServError& /*Error*/);

/** The end of RequestNewRoom: the room, registered and empty — or the platform's typed refusal. */
DECLARE_DELEGATE_ThreeParams(FPlayServRequestNewRoomCallback, bool /*bSuccess*/, const FPlayServRoomListing& /*Room*/, const FPlayServError& /*Error*/);

/** One change to a record of an entity this server subscribed to with SubscribeData, as the platform sends it. */
struct FPlayServDataUpdate
{
	/** The entity (table) the record belongs to. */
	FString Entity;
	/** The record's id, as the platform names it. */
	FString Id;
	/** `upsert` or `delete`. */
	FString Op;
	/** The record's fields: as written for an upsert, as they were for a delete. Null when the platform sent none. */
	TSharedPtr<FJsonObject> Data;

	bool IsDelete() const { return Op == TEXT("delete"); }
};

/** A record of an entity this server subscribed to changed: another server, a function or an operator wrote or deleted it. */
DECLARE_MULTICAST_DELEGATE_OneParam(FPlayServOnDataUpdate, const FPlayServDataUpdate& /*Update*/);

/** An entity's data subscription went out on the uplink. Changes made while it was not in place are not sent again. */
DECLARE_MULTICAST_DELEGATE_OneParam(FPlayServOnDataSubscribed, const FString& /*Entity*/);

/**
 * The Rooms module.
 *
 * Hosting, on a dedicated server: the server registers its rooms with the platform, admits players by the tickets the
 * platform sends it, and reports who is in each room. Presence is automatic: while hosting, the module follows the
 * engine's own login and logout events, so the game never reports a player joining or leaving.
 *
 * Joining, on a client: a signed-in player browses the rooms of a room type, joins one by name and travels with the
 * ticket the platform issues, or asks PlayServ hosting for a new room with RequestNewRoom and then joins it.
 *
 * Call it through the PlayServ::Rooms namespace (PlayServ.h) or UPlayServSubsystem::Get()->GetRooms(). Hosting:
 * StartHosting, then StartRoom per room, VerifyTicket from the game mode's PreLogin, and CloseRoom and StopHosting to
 * finish. A server PlayServ hosting started for one room calls StartRoomPlayServHosted instead of the first two.
 */
UCLASS()
class PLAYSERVRUNTIME_API UPlayServRooms : public UObject
{
	GENERATED_BODY()

public:
	// ---- Hosting: lifecycle -----------------------------------------------------------------

	/**
	 * Sign in as a server when no server session exists, and connect to the platform. OnReady fires once connected, or
	 * with the refusal: no room type configured, no server credential, the platform's refusal, or `room_config_missing`.
	 * A second call while hosting completes at once.
	 */
	void StartHosting(FPlayServSimpleCallback OnReady);

	/**
	 * Close every room, release every ticket and disconnect. Call it when the process ends, not from a game mode's
	 * EndPlay on a map change (EEndPlayReason::LevelTransition): a room outlives ServerTravel, and the next map's game
	 * mode finds it with GetRoom.
	 */
	void StopHosting();

	UFUNCTION(BlueprintPure, Category="PlayServ|Rooms")
	bool IsHosting() const;

	UFUNCTION(BlueprintPure, Category="PlayServ|Rooms")
	EPlayServUplinkState GetUplinkState() const;

	/** True once the platform has sent this server's room configuration. */
	UFUNCTION(BlueprintPure, Category="PlayServ|Rooms")
	bool HasRoomConfig() const;

	UFUNCTION(BlueprintPure, Category="PlayServ|Rooms")
	FPlayServRoomConfig GetRoomConfig() const;

	UFUNCTION(BlueprintPure, Category="PlayServ|Rooms")
	EPlayServAdmissionMode GetAdmissionMode() const;

	/** This server's instance id: the one PlayServ hosting issued when it started the process, else a new GUID. */
	UFUNCTION(BlueprintPure, Category="PlayServ|Rooms")
	FString GetInstanceId() const;

	/** The room type this process registers its rooms under. */
	UFUNCTION(BlueprintPure, Category="PlayServ|Rooms")
	FString GetRoomDefaultSlug() const;

	/** The room PlayServ hosting started this process for; empty for a server the studio runs itself. */
	UFUNCTION(BlueprintPure, Category="PlayServ|Rooms")
	FString GetLaunchRoomName() const;

	// ---- Hosting: rooms ---------------------------------------------------------------------

	/**
	 * Register a room and keep it registered. Needs StartHosting to have completed. The snapshot must carry the address
	 * players connect to (Connect.Host and Connect.Port): the SDK never works it out, and a room without one is refused
	 * `connect_not_declared`. Also refused when the snapshot breaks a rule or the room limit is reached. Callback fires
	 * when the platform first answers for the room.
	 */
	void StartRoom(const FPlayServRoomSnapshot& Snapshot, FPlayServSimpleCallback Callback);

	/**
	 * The one call a server started by PlayServ hosting makes: connects when not connected, then registers the room the
	 * launch asked for, as the launch describes it — its name, the machine's public address with the port the launch
	 * assigned, the attributes the requesting player chose, and the capacity they asked for or else the room
	 * configuration's. GetRoom reads the room back. Refused `not_a_platform_launch` in any other process and
	 * `launch_incomplete` when the launch names no port or address. Callback fires when the platform first answers for
	 * the room; the player who asked for it is waiting for that.
	 */
	void StartRoomPlayServHosted(FPlayServSimpleCallback Callback);

	/** Replace what the platform is told about a room: state, attributes, capacity, open. Matched by RoomName. */
	bool UpdateRoom(const FPlayServRoomSnapshot& Snapshot);

	bool SetRoomOpen(const FString& RoomName, bool bOpen);

	/** Unregister a room and void its tickets. */
	void CloseRoom(const FString& RoomName, FPlayServSimpleCallback Callback);

	UFUNCTION(BlueprintPure, Category="PlayServ|Rooms")
	TArray<FString> GetRoomNames() const;

	/**
	 * A room this process registered, as the platform knows it: attributes, state, address, and the capacity players are
	 * admitted up to. False when this process hosts no such room. A room outlives a map change, so a game mode checks
	 * here before starting it again: registering the same room twice is refused.
	 */
	UFUNCTION(BlueprintPure, Category="PlayServ|Rooms")
	bool GetRoom(const FString& RoomName, FPlayServRoomSnapshot& OutRoom) const;

	UFUNCTION(BlueprintPure, Category="PlayServ|Rooms")
	EPlayServPlacementState GetRoomPlacement(const FString& RoomName) const;

	/** Players the room currently holds, parked ones included. */
	UFUNCTION(BlueprintPure, Category="PlayServ|Rooms")
	int32 GetRoomPlayerCount(const FString& RoomName) const;

	// ---- Hosting: admission and presence -----------------------------------------------------

	/** The room ticket in a travel URL's options (the `rsv` option, any case), or empty. */
	static FString TicketFromOptions(const FString& Options);

	/**
	 * Whether this ticket admits its bearer: the one call a hosting game makes, from AGameModeBase::PreLogin. Answers at
	 * once, with no request. On a refusal copy ErrorMessage into PreLogin's error; the module handles everything after
	 * that on its own.
	 */
	FPlayServTicketVerdict VerifyTicket(const FString& Ticket);

	/**
	 * The PlayServ player id of a connection this server admitted by ticket, also after a seamless travel gives it a new
	 * controller; empty for any other connection, such as a local player or a bot.
	 */
	FString GetPlayerId(const APlayerController* Player) const;

	/**
	 * Admit a player whose ticket VerifyTicket accepted, on a connection the engine does not log in: a WebSocket door,
	 * a beacon, the game's own transport. It does for that player what the engine's PostLogin does for a network login:
	 * the ticket leaves the verified list and the platform hears the join with its reservation token, so the player is
	 * in the room's roster. RemovePlayer is their leave. False when the verdict was a refusal or a development fail-open
	 * admission, when its ticket was never verified here or was admitted already, or when its room is gone.
	 */
	bool AdmitVerified(const FPlayServTicketVerdict& Verdict);

	/** Remove a player by the game's own decision, such as a kick: reported at once, with no reconnect grace. False when the player is not in that room. */
	bool RemovePlayer(const FString& RoomName, const FString& PlayerId);

	/** How long a dropped player keeps their seat before the SDK reports them gone. Default 30 s; the time a map takes to load is not counted. */
	void SetReconnectGraceSeconds(float Seconds);

	float GetReconnectGraceSeconds() const;

	/** Bind to veto a ticket the platform offers for one of this server's rooms. Unbound accepts every offer. */
	FPlayServTicketOfferDecision OnTicketOffer;

	// ---- Hosting: data ----------------------------------------------------------------------

	/**
	 * Hear every change to an entity's records over the uplink, whoever makes it: another server, a cloud function, an
	 * operator. Each upsert and each delete arrives in OnDataUpdate. KeyPath is how the platform keys a record,
	 * `field:<name>` for the field that holds its key. The subscription goes out at once when the uplink is ready,
	 * else when it becomes ready, and again on every new uplink socket; OnDataSubscribed fires each time it goes out.
	 * The platform does not send again what changed while no subscription was in place, so a game that must not miss
	 * a change reads the records again from OnDataSubscribed. A second call for the same entity replaces its key path.
	 */
	void SubscribeData(const FString& Entity, const FString& KeyPath);

	/** Stop hearing an entity's changes. */
	void UnsubscribeData(const FString& Entity);

	/** A record of a subscribed entity changed. Bind before SubscribeData. */
	FPlayServOnDataUpdate OnDataUpdate;

	/** A data subscription went out on the uplink: at SubscribeData on a ready uplink, and on every new uplink socket. */
	FPlayServOnDataSubscribed OnDataSubscribed;

	/**
	 * Write a record over the uplink, as the C# SDK's RuntimeData.Write does: the platform upserts it by its business key
	 * Id (the value of the entity's primary field), Data's fields merged into the row, a new row if none has that key.
	 * There is no version to match (no ETag, no 412 from another writer) and no HTTP round trip; nothing answers. The
	 * platform tells every subscriber of the entity but this server, and tells them before it stores the row, so a read
	 * made at once can still miss it; and two writes a moment apart can reach a subscriber in either order. False, and
	 * nothing sent, while the uplink is not ready: a write is not queued, so a game writes a row it keeps current again.
	 */
	bool WriteData(const FString& Entity, const FString& Id, const TSharedRef<FJsonObject>& Data);

	/** Delete a record over the uplink by its business key, as RuntimeData.Delete does. False while the uplink is not ready. */
	bool DeleteData(const FString& Entity, const FString& Id);

	// ---- Hosting: logs ----------------------------------------------------------------------

	/**
	 * A line in this game server's logs on the platform: the function logs of its game server, where the C# SDK's
	 * Platform.Log lines go (list_function_logs). It goes over the uplink as the C# SDK's `log` frame. A line logged
	 * while the uplink is not ready waits for it, as the C# SDK keeps them on a pool server: the last
	 * MaxPendingLogLines, sent oldest first once it is, after a line counting any dropped. A message longer than
	 * MaxLogMessageChars is cut there. Call on the game thread.
	 */
	void Log(const FString& Message, EPlayServLogLevel Level, const TSharedPtr<FJsonObject>& Data = nullptr);

	/**
	 * Send this process's own UE_LOG lines to the platform's logs as well, by the given rules: which categories, from
	 * which verbosity, and how many lines at most. Lines may be logged on any thread; they go out on the game thread a
	 * moment later, through Log. A second call replaces the rules. The pool launcher does not forward a server's own
	 * output, so on a pool machine this is how UE_LOG reaches the platform at all.
	 */
	void ForwardLogs(const FPlayServLogForwarding& Rules);

	/** Stop ForwardLogs, sending what it had caught. */
	void StopForwardingLogs();

	static constexpr int32 MaxPendingLogLines = 200;
	static constexpr int32 MaxLogMessageChars = 16 * 1024;

	// ---- Joining: client --------------------------------------------------------------------

	/** List a room type's joinable rooms. Needs a client session. Filters and the page cursor are in FPlayServRoomFilters. */
	void Browse(const FString& Slug, const FPlayServRoomFilters& Filters, FPlayServBrowseCallback Callback);

	/** The same for the project's room type in the settings (RoomDefaultSlug). Refused when none is configured. */
	void Browse(const FPlayServRoomFilters& Filters, FPlayServBrowseCallback Callback);

	/**
	 * Join a room by name and travel to it. A ticket lives about ten seconds, so ask the player anything before this
	 * call, not after. With Player set the SDK travels as soon as the ticket is usable; with Player null the ticket
	 * comes back for the game to travel with.
	 */
	void JoinRoom(const FString& Slug, const FString& RoomName, APlayerController* Player, FPlayServJoinCallback Callback);

	/** The same for the project's room type in the settings (RoomDefaultSlug). Refused when none is configured. */
	void JoinRoom(const FString& RoomName, APlayerController* Player, FPlayServJoinCallback Callback);

	/**
	 * Ask PlayServ hosting for a new room of the project's room type (RoomDefaultSlug), with the settings the player
	 * chose as attributes: one level of strings, at most 2048 bytes; the keys the SDK or a server image acts on are in
	 * PlayServ::Rooms::Attributes, and a Capacity that is not a seat count from 1 to 1000 is refused `capacity_invalid`.
	 * The platform starts a server for the room when it can and answers once that server has registered it, typically
	 * in seconds and within 90.
	 *
	 * The room comes back registered and empty, as Browse lists it: this call joins nothing, so a Host button calls
	 * RequestNewRoom, then JoinRoom with Room.RoomName. Refusals, in Error.ProblemCode: `room_host_unavailable` (no
	 * hosting for this room type, or the server did not start), `room_host_capacity_exhausted` (every server busy; retry
	 * in a few seconds), `room_unreachable` (the server never registered the room), `room_type_not_found`. Needs a
	 * client session and a configured room type.
	 */
	void RequestNewRoom(const TMap<FString, FString>& Attributes, FPlayServRequestNewRoomCallback Callback);

	/**
	 * The travel URL for a ticket: `host:port?rsv=<token>`, with the room's connect string in place of `host:port` when
	 * it declared one. For a game that travels by itself; JoinRoom already travels with it.
	 */
	static FString BuildTravelUrl(const FPlayServRoomTicket& Ticket);

	// ---- Events -----------------------------------------------------------------------------

	UPROPERTY(BlueprintAssignable, Category="PlayServ|Rooms")
	FPlayServOnUplinkStateChanged OnUplinkStateChanged;

	UPROPERTY(BlueprintAssignable, Category="PlayServ|Rooms")
	FPlayServOnRoomConfigChanged OnRoomConfigChanged;

	UPROPERTY(BlueprintAssignable, Category="PlayServ|Rooms")
	FPlayServOnRoomPlacementChanged OnRoomPlacementChanged;

	/** The SDK ended a room: `lifetime`, `idle`, `room_owned_by_other_instance`, `instance_id_mismatch`, `room_type_not_found`. */
	UPROPERTY(BlueprintAssignable, Category="PlayServ|Rooms")
	FPlayServOnRoomEnded OnRoomEnded;

	/** The SDK removed a player from a room: the reconnect grace ran out, the game removed them, or the platform refused their join. Disconnect them. */
	UPROPERTY(BlueprintAssignable, Category="PlayServ|Rooms")
	FPlayServOnPlayerRemoved OnPlayerRemoved;

private:
	friend class UPlayServSubsystem;

#if !UE_BUILD_SHIPPING
	friend class FPlayServRoomsTestAccess;
#endif

	void Init(TSharedPtr<FPlayServHttp> InHttp);
	void Shutdown();

private:
	struct FPendingStart
	{
		FPlayServSimpleCallback Callback;
	};

	struct FLaunch
	{
		FString RoomName;
		int32 ListenPort = 0;
		FString PublicHost;
		TMap<FString, FString> Attributes;

		bool IsSet() const { return !RoomName.IsEmpty(); }
	};

	struct FVerifiedTicket
	{
		FString PlayerId;
		FString RoomName;
		double VerifiedAt = 0.0;
		bool bResume = false;
	};

	void OpenUplink();
	void HandleUplinkState(EPlayServUplinkState State);
	void HandleUplinkReady(const FPlayServUplinkAck& Ack, int32 Generation);
	void HandleUplinkFrame(const FString& Type, const TSharedPtr<FJsonObject>& Frame);
	void HandleUplinkRefused(const FString& Reason, bool bPermanent);
	void HandleTicketOffer(const TSharedPtr<FJsonObject>& Frame);
	void HandleJoinAck(const TSharedPtr<FJsonObject>& Frame);
	void HandleDataUpdate(const TSharedPtr<FJsonObject>& Frame);
	/** Sends one data subscription on the ready uplink and reports it through OnDataSubscribed. */
	bool SendDataSubscription(const FString& Entity, const FString& KeyPath);
	/** Sends one data_write frame (WriteData, DeleteData) on the ready uplink. */
	bool SendDataWrite(const FString& Entity, const FString& Id, const TCHAR* Op, const TSharedRef<FJsonObject>& Data);
	/** Sends the log lines waiting for the uplink, oldest first, while it is ready. */
	void FlushPendingLogs();
	bool TickLogForwarding(float DeltaTime);
	void ApplyRoomConfig(const FPlayServRoomConfig& Config, const TCHAR* Source);

	bool TickMaintenance(float DeltaTime);
	void TickRooms(double Now);
	void SendHeartbeat(const FString& RoomName, double Now);
	void HandleHeartbeatAnswer(const FString& RoomName, bool bSuccess, int32 Status, const FString& ProblemCode, const TSharedPtr<FJsonObject>& Json, const FPlayServError& Error);
	void SendRosterRepair(FPlayServRoomRuntime& Room);
	bool SendPresenceDelta(FPlayServRoomRuntime& Room, const TCHAR* Event, const FString& PlayerId, const FString& ReservationToken);
	void EndRoom(const FString& RoomName, const FString& Reason, bool bSendClose);
	void CompletePendingStart(bool bSuccess, const FPlayServError& Error);
	double Now() const;

	bool ReleaseTicket(const FString& ReservationToken, const FString& Reason, const FString& Detail, bool bForget = true);

	bool ReportPlayerLeft(const FString& RoomName, const FString& PlayerId);

	void BindGameModeEvents();
	void UnbindGameModeEvents();
	void HandleGameModePostLogin(AGameModeBase* GameMode, APlayerController* NewPlayer);
	void HandleGameModeLogout(AGameModeBase* GameMode, AController* Exiting);

	void AdmitLogin(const APlayerController* Player, const FString& RequestUrl);
	void DropLogin(const APlayerController* Player);

	static FString RequestUrlOf(const APlayerController* Player);

	static TArray<const APlayerController*> HostedPlayerControllers();

	void HandlePreLoadMap(const FString& MapName);

	void HandlePostLoadMap(UWorld* LoadedWorld);

	FString ResolvePlayerId(const APlayerController* Player) const;

	bool FindMemberByTicket(const FString& ReservationToken, FString& OutRoomName, FString& OutPlayerId) const;

	const APlayerController* FindLiveController(const FString& PlayerId, const FString& ReservationToken, const APlayerController* Except) const;

	void HoldSeat(const FString& PlayerId, const APlayerController* Dropped);

	void SweepLostControllers();

	void AdmitToRoom(const FString& RoomName, const FString& PlayerId, const FString& ReservationToken);

	const FVerifiedTicket* FindVerified(const FString& ReservationToken) const;

	void RememberVerified(const FPlayServTicketVerdict& Verdict, double Current, bool bResume = false);

	void SweepVerified(double Current);

	void RequestNewRoomOfType(const FString& Slug, const TMap<FString, FString>& Attributes, FPlayServRequestNewRoomCallback Callback);

	TSharedPtr<FPlayServJoinRound> FindJoin(int32 Id) const;
	void StartJoinRound(TSharedPtr<FPlayServJoinRound> Round);
	void SendNamedJoin(TSharedPtr<FPlayServJoinRound> Round);
	void ApplyJoinDecision(TSharedPtr<FPlayServJoinRound> Round, const PlayServRoomsClientWire::FJoinStepDecision& Decision, double Current);
	void TakeJoinStep(TSharedPtr<FPlayServJoinRound> Round);
	void HandleMatched(TSharedPtr<FPlayServJoinRound> Round, const FPlayServRoomTicket& Ticket);
	void FinishJoin(TSharedPtr<FPlayServJoinRound> Round, const FPlayServRoomTicket& Ticket, bool bTravelled, const FPlayServError& Error);
	bool TickJoins(double Current);

	FPlayServError RequireClientSession() const;

	static FPlayServError RequireRoomDefaultSlug(FString& OutSlug);
	static FString ResolveCredential(const FString& DeploymentToken, const FString& ServerKey);
	static FString ResolveInstanceId(const FString& DeploymentToken);
	static FLaunch ReadLaunch();
	static FPlayServError SnapshotFromLaunch(const FLaunch& InLaunch, FPlayServRoomSnapshot& OutSnapshot);

	TSharedPtr<FPlayServHttp> Http;
	TSharedPtr<FPlayServUplinkClient> Uplink;
	TSharedPtr<FPlayServAdmissionTable> Tickets;
	TMap<FString, TSharedPtr<FPlayServRoomRuntime>> Rooms;
	TMap<FString, FPlayServSimpleCallback> PendingRoomStarts;
	TArray<FPendingStart> PendingStarts;

	TMap<FString, FVerifiedTicket> Verified;

	TMap<TWeakObjectPtr<const APlayerController>, FString> AdmittedPlayers;

	/** The entities SubscribeData asked for, and each one's key path; sent again on every new uplink socket. */
	TMap<FString, FString> DataSubscriptions;
	/** The entities a data_update has arrived for, so the first one of each is logged. */
	TSet<FString> DataHeard;
	/** The entities a write has gone out for, so the first one of each is logged. */
	TSet<FString> DataWritten;
	/** The uplink frame types this module does not serve that have arrived, so the first of each is logged. */
	TSet<FString> UnknownFrames;

	/** Log frames waiting for the uplink to be ready, oldest first, and how many were dropped from the front. */
	TArray<TSharedPtr<FJsonObject>> PendingLogs;
	int32 DroppedLogLines = 0;
	/** The device that catches UE_LOG lines for ForwardLogs, and the ticker that sends what it caught. */
	TSharedPtr<class FPlayServLogForwarder> LogForwarder;
	FTSTicker::FDelegateHandle LogForwardingTickerHandle;

	TMap<int32, TSharedPtr<FPlayServJoinRound>> Joins;
	int32 NextJoinId = 1;

	FString RoomDefaultSlug;
	FString InstanceId;
	FLaunch Launch;
	FPlayServRoomConfig RoomConfig;
	bool bHasRoomConfig = false;
	bool bHosting = false;
	EPlayServAdmissionMode AdmissionMode = EPlayServAdmissionMode::None;
	// Warning: a connection admitted fail-open gets no PlayServ identity; a player id comes from the platform, never from the travel URL.
	bool bAdmissionFailOpen = false;
	float ReconnectGraceSeconds = 30.0f;
	int32 UplinkGeneration = 0;

	TFunction<double()> Clock;
	TFunction<TSharedPtr<class FPlayServUplinkTransport>(const FString&, const FString&)> TransportFactory;
	TFunction<FString(const APlayerController*)> ConnectionUrlOf = &UPlayServRooms::RequestUrlOf;
	TFunction<TArray<const APlayerController*>()> LivePlayerControllers = &UPlayServRooms::HostedPlayerControllers;
	double LoadStartedAt = 0.0;
	FTSTicker::FDelegateHandle MaintenanceTickerHandle;
	FDelegateHandle PostLoginHandle;
	FDelegateHandle LogoutHandle;
	FDelegateHandle PreLoadMapHandle;
	FDelegateHandle PostLoadMapHandle;
};
