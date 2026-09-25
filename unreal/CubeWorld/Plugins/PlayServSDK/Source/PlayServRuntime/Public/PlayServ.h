#pragma once

#include "CoreMinimal.h"
#include "Core/PlayServTypes.h"
#include "Core/PlayServSubsystem.h"
#include "Auth/PlayServAuth.h"
#include "Data/PlayServData.h"
#include "Data/PlayServFilter.h"
#include "Code/PlayServCode.h"
#include "Code/PlayServRPCParams.h"
#include "Code/PlayServRpcConverter.h"
#include "Rooms/PlayServRoomsTypes.h"
#include "Rooms/PlayServRooms.h"

/**
 * PlayServ SDK public API. Every operation lives in the PlayServ namespace; entity classes are marked
 * UCLASS(PlayServEntity) and get no methods of their own.
 *
 * Usage:
 *   #include "PlayServ.h"
 *
 *   FPlayServExternalCredential Cred;
 *   Cred.Type = EPlayServExternalAuthType::EpicAccessToken;
 *   Cred.Token = EpicAccessToken; // obtained by the caller from its EOS OnlineSubsystem
 *   PlayServ::Auth::LoginExternal(Cred,
 *       FPlayServAuthCallback::CreateLambda([](bool bOk, const FString& PlayerId, const FPlayServError& Err) { ... }));
 *
 *   UMyPlayer* P = PlayServ::Data::Create<UMyPlayer>();
 *   PlayServ::Data::Save(P, FPlayServSimpleCallback::CreateLambda(
 *       [](bool bOk, const FPlayServError& Err) { ... }));
 *
 *   PlayServ::Code::Call(TEXT("myFunction"), Params,
 *       FPlayServRPCCallback::CreateLambda(
 *           [](bool bOk, const FPlayServRPCResult& Result, const FPlayServError& Err) { ... }));
 */

namespace PlayServ::Auth
{
	/**
	 * Create a new guest player and sign them in. Every call creates a new player; to resume the same guest later, keep
	 * the refresh token (SetRefreshTokenChangedHandler) and sign in with LoginWithRefreshToken.
	 */
	PLAYSERVRUNTIME_API void LoginAnonymous(FPlayServAuthCallback Callback);

	/**
	 * The same, naming the new player so they do not show as blank wherever the platform lists players.
	 *
	 *   PlayServ::Auth::LoginAnonymous(EnteredNickname, Callback);
	 *
	 * The name is trimmed and cut to 64 characters, never refused, and left out when nothing remains. A later login
	 * cannot change it.
	 */
	PLAYSERVRUNTIME_API void LoginAnonymous(const FString& DisplayName, FPlayServAuthCallback Callback);

	/**
	 * Sign in with a credential from an external provider that the game has obtained: EpicAccessToken,
	 * EpicLauncherExchangeCode or SteamWebApiTicket (EPlayServExternalAuthType). A credential of type None fails at once,
	 * with no request.
	 */
	PLAYSERVRUNTIME_API void LoginExternal(const FPlayServExternalCredential& Credential, FPlayServAuthCallback Callback);

	/**
	 * When the Epic Games Launcher started this process with an exchange code, fill OutCredential with an
	 * EpicLauncherExchangeCode credential for LoginExternal and return true; otherwise return false and leave it
	 * untouched. The launcher code and a native EOS token lead to different platform identities, so which one to sign
	 * in with is the game's decision.
	 *
	 *   FPlayServExternalCredential Cred;
	 *   if (PlayServ::Auth::TryGetLauncherCredential(Cred))
	 *   {
	 *       PlayServ::Auth::LoginExternal(Cred, Callback);   // Epic Games Store entry
	 *   }
	 *   else
	 *   {
	 *       // ... acquire a native EOS Connect token and log in with EpicAccessToken
	 *   }
	 *
	 * Safe to call before the SDK has started.
	 */
	PLAYSERVRUNTIME_API bool TryGetLauncherCredential(FPlayServExternalCredential& OutCredential);

	/**
	 * Hex-encode a Steam web-API ticket, as Steamworks returns it, into a SteamWebApiTicket credential. Returns false,
	 * leaving OutCredential untouched, for an empty ticket or one longer than Steam mints.
	 *
	 *   // Steamworks: GetTicketForWebApiResponse_t handed to your callback
	 *   FPlayServExternalCredential Cred;
	 *   if (PlayServ::Auth::TryMakeSteamCredential(
	 *           MakeArrayView(Response.m_rgubTicket, Response.m_cubTicket), Cred))
	 *   {
	 *       PlayServ::Auth::LoginExternal(Cred, Callback);
	 *   }
	 *
	 * Through OnlineSubsystemSteam the ticket is already hex: set Type and Token yourself. Which Steam call mints a
	 * usable ticket is easy to get wrong without any local error; see EPlayServExternalAuthType::SteamWebApiTicket.
	 */
	PLAYSERVRUNTIME_API bool TryMakeSteamCredential(TConstArrayView<uint8> TicketBytes, FPlayServExternalCredential& OutCredential);

	/**
	 * Resume the player of an earlier session with the refresh token kept from it. This is how a guest keeps their
	 * profile and progress across launches. The SDK stores nothing: the game keeps the token wherever the platform it
	 * runs on keeps credentials.
	 *
	 *   // at startup, with whatever the game stored last session
	 *   PlayServ::Auth::LoginWithRefreshToken(StoredToken,
	 *       FPlayServAuthCallback::CreateLambda([](bool bOk, const FString& PlayerId, const FPlayServError& Err)
	 *       {
	 *           if (bOk) { return; }
	 *
	 *           if (Err.Code == EPlayServErrorCode::NetworkUnreachable
	 *               || Err.Code == EPlayServErrorCode::Timeout)
	 *           {
	 *               return;   // offline: the platform never saw the token, so keep it
	 *           }
	 *           MyStore.Erase();   // refused: the token is spent
	 *           PlayServ::Auth::LoginAnonymous(...);
	 *       }));
	 *
	 * Take the token from SetRefreshTokenChangedHandler, which hands it over at login and on every rotation, about every
	 * 12 minutes of play. It is single-use: presenting a spent one makes the platform revoke that session, so never
	 * retry a token and never pass one a live session is using. A call while another refresh-token exchange is running
	 * fails at once, with no request.
	 */
	PLAYSERVRUNTIME_API void LoginWithRefreshToken(const FString& RefreshToken, FPlayServAuthCallback Callback);

	/** Start a server session with the studio's sk_* server key or a deployment token. No request is made. */
	PLAYSERVRUNTIME_API void LoginServer(const FString& ApiKey, FPlayServSimpleCallback Callback);

	/**
	 * Start a server session with the server key from the SDK settings (Config/DedicatedServerGame.ini, or
	 * -PlayServServerKey= / PLAYSERV_SERVER_KEY). Fails when no key is configured.
	 */
	PLAYSERVRUNTIME_API void LoginServer(FPlayServSimpleCallback Callback);

	/**
	 * End the current session. Revoke, the default, also signs a client session out on the platform; Local keeps the
	 * refresh token valid, so LoginWithRefreshToken resumes the same player. Sign a guest out with Local: their refresh
	 * token is their only way back in.
	 */
	PLAYSERVRUNTIME_API void Logout(FPlayServSimpleCallback Callback, EPlayServLogoutMode Mode = EPlayServLogoutMode::Revoke);

	/** True while a client or server session exists. */
	PLAYSERVRUNTIME_API bool IsLoggedIn();

	/** True while the session is a server session. */
	PLAYSERVRUNTIME_API bool IsServerSession();

	/** The current session type. */
	PLAYSERVRUNTIME_API EPlayServSessionType GetSessionType();

	/** The signed-in player's id; empty with no session or a server session. */
	PLAYSERVRUNTIME_API const FString& GetPlayerId();

	/** Broadcast when the platform refuses the session's refresh token and the session is cleared; not when a refresh fails to reach the platform. */
	PLAYSERVRUNTIME_API FPlayServOnSessionLost& OnSessionLost();

	/**
	 * Install the handler that receives the refresh token every time it changes, at login and on every rotation, so the
	 * game can keep it for LoginWithRefreshToken. Store it synchronously in the handler: a process that dies before the
	 * write leaves a spent token behind. Never called for a server session, on logout or on session loss; clear the
	 * stored copy from Logout's callback and from OnSessionLost.
	 *
	 *   PlayServ::Auth::SetRefreshTokenChangedHandler(
	 *       FPlayServRefreshTokenChanged::CreateUObject(this, &UMyGameInstance::PersistRefreshToken));
	 *
	 * One handler; a second call replaces it. Not Blueprint-assignable — it carries a credential.
	 */
	PLAYSERVRUNTIME_API void SetRefreshTokenChangedHandler(FPlayServRefreshTokenChanged Handler);
}

namespace PlayServ::Data
{
	/**
	 * Create an entity in memory, without contacting the platform. It has no record id until its first Save. Its outer
	 * is the transient package; Rename it onto an owner of yours if you need one. Does not compile for a singleton or a
	 * player-owned class.
	 */
	template<typename T>
	T* Create();

	/** Load one entity by the record id the platform minted for it (GetRecordId of a saved or loaded entity). */
	template<typename T>
	void Load(const FString& RecordId, TFunction<void(bool, T*, const FPlayServError&)> Callback);

	/**
	 * Load the one row of a singleton class, UCLASS(PlayServSingleton): game config or world state, one row per project
	 * and environment. The platform creates the row with its field defaults the first time anyone reads it, so a missing
	 * row is never an error.
	 *
	 *   PlayServ::Data::LoadSingleton<UGameConfig>([](bool bOk, UGameConfig* Config, const FPlayServError& Err) { ... });
	 *
	 * Every call returns a new instance. Save sends the fields changed since this load; PreconditionFailed means another
	 * writer changed the row first, so Reload, apply the change again and Save. Reload reads the row again. Delete and
	 * Subscribe refuse a singleton, and Create, Load, LoadAll, DeleteById and DeleteAll do not compile for one. Clients
	 * read a singleton when its table is open to client reads; servers read and write it.
	 */
	template<typename T>
	void LoadSingleton(TFunction<void(bool, T*, const FPlayServError&)> Callback);

	/**
	 * Load a player's row of a player-owned class, UCLASS(PlayServEntity, PlayServPlayerOwned): one row per player, owned
	 * by that player on the platform.
	 *
	 *   PlayServ::Data::LoadPlayerOwned<UPlayerProfile>(PlayerController, [](bool bOk, UPlayerProfile* Row, bool bCreated, const FPlayServError& Err) { ... });
	 *
	 * On a client the controller is the local player's and the row is the signed-in player's. When it is missing and the
	 * table is open to client writes, the call creates it with the class defaults and bCreated is true. On a dedicated
	 * server the player is the one PlayServ verified for that connection (PlayServ::Rooms::GetPlayerId), never an id the
	 * client sent; a server loads and saves the row but does not create it. A missing row the call does not create is
	 * reported as (false, nullptr, false) with Error.Code NotFound: create it on the player's side, for example in a cloud
	 * function the client calls after login.
	 *
	 * The table keeps each row's player in a text field named player_id, unique and required. The SDK writes it; the
	 * class does not declare it.
	 */
	template<typename T>
	void LoadPlayerOwned(const APlayerController* Player, TFunction<void(bool, T*, bool, const FPlayServError&)> Callback);

	/**
	 * The same for a player id, for a flow with no controller: a save after the player left, a job on the server. On a
	 * client the id must be the signed-in player's. On a server the game must have verified it.
	 */
	template<typename T>
	void LoadPlayerOwned(const FString& PlayerId, TFunction<void(bool, T*, bool, const FPlayServError&)> Callback);

	/** Load every entity of type T that matches the filter. */
	template<typename T>
	void LoadAll(const FPlayServFilter& Filter, TFunction<void(bool, TArray<T*>, const FPlayServError&)> Callback);

	/** Delete an entity by its record id, with no instance needed. */
	template<typename T>
	void DeleteById(const FString& RecordId, FPlayServSimpleCallback Callback);

	/** Delete every entity of type T that matches the filter. Deleting all of them takes FPlayServFilter::All(). */
	template<typename T>
	void DeleteAll(const FPlayServFilter& Filter, FPlayServDeleteAllCallback Callback);

	/**
	 * Save an entity. One with no record id is created, and the platform mints its id; otherwise only the fields changed
	 * since the last load or save are sent. A singleton sends its changed fields, and an instance that LoadSingleton or
	 * Reload did not produce is refused. A player-owned instance with no record id is refused: get it with
	 * LoadPlayerOwned.
	 */
	PLAYSERVRUNTIME_API void Save(UObject* Entity, FPlayServSimpleCallback Callback);

	/**
	 * Re-fetch the entity and overwrite its persisted fields. The PlayServOnChanged function of each field whose value
	 * changed, then the instance's Subscribe delegates, run before Callback. A singleton reads its one row.
	 */
	PLAYSERVRUNTIME_API void Reload(UObject* Entity, FPlayServSimpleCallback Callback);

	/** Delete this entity on the platform. Refused for a singleton, whose row belongs to the table. */
	PLAYSERVRUNTIME_API void Delete(UObject* Entity, FPlayServSimpleCallback Callback);

	/** Fill a stub entity, one with a record id and no data yet, from the platform. */
	PLAYSERVRUNTIME_API void Populate(UObject* Entity, FPlayServSimpleCallback Callback);

	/** Fill every stub entity in an array property with one request. */
	PLAYSERVRUNTIME_API void PopulateArray(UObject* Entity, const FString& PropertyName, FPlayServSimpleCallback Callback);

	/** Save every entity in the array, each as Save would. New entities have their record ids in the callback. */
	PLAYSERVRUNTIME_API void BulkSave(const TArray<UObject*>& Entities, FPlayServSimpleCallback Callback);

	/** Delete every entity in the array, one request each. */
	PLAYSERVRUNTIME_API void BulkDelete(const TArray<UObject*>& Entities, FPlayServSimpleCallback Callback);

	/** The record id the platform minted for this entity, or empty before its first Save. */
	PLAYSERVRUNTIME_API FString GetRecordId(const UObject* Entity);

	/**
	 * Keep a loaded entity current with the platform and be told when it changes. When the platform sends the record's
	 * latest state, the SDK overwrites the entity's persisted fields, as Reload does, calls the PlayServOnChanged
	 * function of each field whose value changed, then calls the delegate with the names of the changed top-level
	 * fields. Only the latest state is delivered, not every change.
	 *
	 * Needs an entity with a record id and a client session; a server session cannot subscribe. Otherwise the handle is
	 * invalid (IsValid() is false). The subscription holds the entity weakly: keep it alive yourself, and Unsubscribe
	 * when done. It ends by itself when the record is deleted, and Logout ends every subscription.
	 */
	PLAYSERVRUNTIME_API FPlayServSubscriptionHandle Subscribe(UObject* Entity, FOnPlayServObjectChanged Delegate);

	/** Keep-current only: the same with no delegate. The class's PlayServOnChanged functions still run. */
	PLAYSERVRUNTIME_API FPlayServSubscriptionHandle Subscribe(UObject* Entity);

	/** End one subscription. Safe on an invalid or already ended handle. */
	PLAYSERVRUNTIME_API void Unsubscribe(FPlayServSubscriptionHandle Handle);
}

namespace PlayServ::Code
{
	/**
	 * Call a cloud function with a USTRUCT request and read its reply into a USTRUCT response. The recommended form.
	 * Field names are sent as declared and must match the function's C# property names exactly.
	 *
	 *  - The request does not serialize: Callback(false, {}, ContractMismatch), with no request sent.
	 *  - The call fails: Callback(false, {}, the error).
	 *  - The reply does not convert: Callback(false, {}, ContractMismatch), never a default-filled success.
	 *  - No reply body: Callback(true, a default TResponse, Success), for functions that return nothing.
	 *  - A field of the wrong type fails the whole reply; a missing field keeps its C++ default.
	 *
	 * Usage:
	 *   PlayServ::Code::Call<FBuyUpgradeRequest, FBuyUpgradeResponse>(TEXT("BuyUpgrade"), Request,
	 *       [](bool bOk, const FBuyUpgradeResponse& Resp, const FPlayServError& Err) { ... });
	 */
	template<typename TRequest, typename TResponse>
	void Call(const FString& FunctionName, const TRequest& Request, TFunction<void(bool, const TResponse&, const FPlayServError&)> Callback);

	/**
	 * The same for a function whose reply you do not need: the reply is ignored and the callback gets only the outcome.
	 *
	 * Usage:
	 *   PlayServ::Code::Call<FRecordEventRequest>(TEXT("RecordEvent"), Request,
	 *       [](bool bOk, const FPlayServError& Err) { ... });
	 */
	template<typename TRequest>
	void Call(const FString& FunctionName, const TRequest& Request, TFunction<void(bool, const FPlayServError&)> Callback);

	/**
	 * Call a cloud function with parameters built at runtime and get its raw JSON reply in FPlayServRPCResult. Use it
	 * only when the request or the reply does not fit a USTRUCT; nothing checks the reply against a type.
	 *
	 * @param FunctionName  The function's slug.
	 * @param Params        The parameters, built with FPlayServRPCParams.
	 * @param Callback      The reply, or the error.
	 */
	PLAYSERVRUNTIME_API void Call(const FString& FunctionName, const FPlayServRPCParams& Params, FPlayServRPCCallback Callback);
}

/**
 * Rooms: a dedicated server hosts rooms, and a client joins one. Every call goes to UPlayServRooms.
 *
 * A hosting game mode:
 *   PlayServ::Rooms::StartHosting(OnReady);                     // once per process
 *   PlayServ::Rooms::StartRoom(Snapshot, OnRegistered);         // per room
 *   // …or, in a server PlayServ hosting started, one call that takes everything from the launch:
 *   PlayServ::Rooms::StartRoomPlayServHosted(OnRegistered);
 *   // PreLogin — the one call the server side needs:
 *   const FPlayServTicketVerdict Verdict = PlayServ::Rooms::VerifyTicket(PlayServ::Rooms::TicketFromOptions(Options));
 *   if (!Verdict.bAccepted) { ErrorMessage = Verdict.ErrorMessage; return; }
 *   // Nothing in PostLogin: the SDK follows the engine's own login events from here.
 *
 * A joining client:
 *   PlayServ::Rooms::Browse(TEXT("arena"), Filters, OnPage);
 *   PlayServ::Rooms::JoinRoom(TEXT("arena"), RoomName, GetPlayerController(), OnJoined);
 *   // A Host button: request a room, then join it like any other — two calls, both the game's.
 *   PlayServ::Rooms::RequestNewRoom({ { PlayServ::Rooms::Attributes::Map, TEXT("Arena") } }, OnRoomReady);
 */
namespace PlayServ::Rooms
{
	// ---- Hosting ---------------------------------------------------------------------------

	/** Open the uplink, starting a server session first when there is none. OnReady fires once PlayServ accepts this server, or with the refusal. */
	PLAYSERVRUNTIME_API void StartHosting(FPlayServSimpleCallback OnReady);

	/** Close every room and the uplink, and stop following the engine's logins. For the end of the process, not a map change: the room outlives it. */
	PLAYSERVRUNTIME_API void StopHosting();

	/** True from StartHosting, once it has a server session, until StopHosting; a failure OnReady reports, such as room_config_missing, does not end it. */
	PLAYSERVRUNTIME_API bool IsHosting();

	/** The room PlayServ hosting started this process for; empty for a server the studio runs itself. */
	PLAYSERVRUNTIME_API FString GetLaunchRoomName();

	/** Register a room and start its heartbeat; Callback fires on the first heartbeat's answer. */
	PLAYSERVRUNTIME_API void StartRoom(const FPlayServRoomSnapshot& Snapshot, FPlayServSimpleCallback Callback);

	/** In a server PlayServ hosting started: open the uplink and register the room it was started for, all from the launch environment. Refused in any other process. */
	PLAYSERVRUNTIME_API void StartRoomPlayServHosted(FPlayServSimpleCallback Callback);

	/** Replace what the next heartbeat carries. */
	PLAYSERVRUNTIME_API bool UpdateRoom(const FPlayServRoomSnapshot& Snapshot);

	/** A room this process registered, as its next heartbeat carries it; false when it hosts no such room. The room outlives a map change — check here before starting it again. */
	PLAYSERVRUNTIME_API bool GetRoom(const FString& RoomName, FPlayServRoomSnapshot& OutRoom);

	/** Unregister a room. */
	PLAYSERVRUNTIME_API void CloseRoom(const FString& RoomName, FPlayServSimpleCallback Callback);

	/** The room ticket in a travel URL's options (the `rsv` option), or empty. */
	PLAYSERVRUNTIME_API FString TicketFromOptions(const FString& Options);

	/** The admission decision, for AGameModeBase::PreLogin. Synchronous, no network. */
	PLAYSERVRUNTIME_API FPlayServTicketVerdict VerifyTicket(const FString& Ticket);

	/** The PlayServ player id of a connection this server admitted by ticket, also after a seamless travel; empty for any other connection. */
	PLAYSERVRUNTIME_API FString GetPlayerId(const APlayerController* Player);

	/** The game's own removal decision (kick, ban, quit): reported at once, with no reconnect grace. */
	PLAYSERVRUNTIME_API bool RemovePlayer(const FString& RoomName, const FString& PlayerId);

	// ---- Joining ---------------------------------------------------------------------------

	/** List this room type's joinable rooms for the signed-in player. */
	PLAYSERVRUNTIME_API void Browse(const FString& Slug, const FPlayServRoomFilters& Filters, FPlayServBrowseCallback Callback);

	/** The same, for the project's default room type (settings `RoomDefaultSlug`). Fails when none is configured. */
	PLAYSERVRUNTIME_API void Browse(const FPlayServRoomFilters& Filters, FPlayServBrowseCallback Callback);

	/** Join a room by name and travel to it. Ask the player anything before this call, not after: a ticket lives about ten seconds. */
	PLAYSERVRUNTIME_API void JoinRoom(const FString& Slug, const FString& RoomName, APlayerController* Player, FPlayServJoinCallback Callback);

	/** The same, for the project's default room type (settings `RoomDefaultSlug`). Fails when none is configured. */
	PLAYSERVRUNTIME_API void JoinRoom(const FString& RoomName, APlayerController* Player, FPlayServJoinCallback Callback);

	/** Request a new room of the project's room type (settings `RoomDefaultSlug`) from PlayServ hosting. It comes back registered and empty — join it with JoinRoom. */
	PLAYSERVRUNTIME_API void RequestNewRoom(const TMap<FString, FString>& Attributes, FPlayServRequestNewRoomCallback Callback);

	/** The travel URL for a ticket, `host:port?rsv=<token>`. Only a game that travels by itself needs it. */
	PLAYSERVRUNTIME_API FString BuildTravelUrl(const FPlayServRoomTicket& Ticket);
}

#include "Data/PlayServDataTemplates.inl"
#include "Code/PlayServCodeTemplates.inl"
