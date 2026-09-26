#pragma once

#include "CoreMinimal.h"
#include "Containers/ArrayView.h"
#include "Containers/Ticker.h"
#include "Core/PlayServTypes.h"
#include "PlayServAuth.generated.h"

class FJsonObject;

/** Login result: the player id on success, the error on failure. */
DECLARE_DELEGATE_ThreeParams(FPlayServAuthCallback, bool /*bSuccess*/, const FString& /*PlayerId*/, const FPlayServError& /*Error*/);

/**
 * Broadcast when a client session is lost for good: the platform refused the session's refresh token, so no stored
 * copy of it can restore the session. Not broadcast when a refresh fails to reach the platform; those are retried and
 * the session is kept. Server sessions never fire it.
 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FPlayServOnSessionLost);

/**
 * Receives the session's refresh token every time it changes; see UPlayServAuth::SetRefreshTokenChangedHandler. Not
 * Blueprint-assignable, because it carries a credential. The argument stays valid for the whole call, even when the
 * handler calls Logout.
 */
DECLARE_DELEGATE_OneParam(FPlayServRefreshTokenChanged, const FString& /*RefreshToken*/);

/**
 * What Logout does with the session's refresh token. An anonymous player has no other credential, so revoking the
 * token ends that account for good: sign a guest out with Local to resume them later with LoginWithRefreshToken.
 */
UENUM(BlueprintType)
enum class EPlayServLogoutMode : uint8
{
	/** Revoke this session on the platform: its refresh token stops working. Other devices' sessions are untouched. */
	Revoke,

	/**
	 * End the session in this process only. Nothing is sent, so the refresh token stays valid for its lifetime and
	 * whoever holds it can resume the same player: keep it only where the player expects to be remembered.
	 */
	Local
};

/**
 * External-login credential type for LoginExternal. The game obtains the credential; the SDK sends it as it is. The
 * two Epic values lead to different platform identities, so a player signed in one way is not the player signed in
 * the other.
 */
UENUM(BlueprintType)
enum class EPlayServExternalAuthType : uint8
{
	/** Unset / invalid. LoginExternal fails fast with no network call. */
	None,

	/** An Epic Account Services access token. The platform identity is the player's Product User ID. */
	EpicAccessToken,

	/**
	 * The Epic Games Launcher's one-time exchange code; get it with TryGetLauncherCredential. The platform identity is
	 * the player's Epic Account ID. Single-use: a failed login cannot be retried with the same code, and a new one needs
	 * a new launch from the launcher.
	 */
	EpicLauncherExchangeCode,

	/**
	 * A Steam ticket for Valve's web API, hex-encoded (TryMakeSteamCredential encodes the bytes Steamworks returns).
	 * The platform identity is the player's SteamID. Mint it with ISteamUser::GetAuthTicketForWebApi and no identity
	 * string: a session ticket, an app ticket or a ticket bound to an identity is refused by the platform, and none of
	 * them can be told apart locally. Docs/Authentication.md walks through getting one.
	 */
	SteamWebApiTicket
};

/** Credential for PlayServ::Auth::LoginExternal. */
USTRUCT(BlueprintType)
struct PLAYSERVRUNTIME_API FPlayServExternalCredential
{
	GENERATED_BODY()

	/** Which credential Token carries. */
	UPROPERTY(BlueprintReadWrite, Category="PlayServ|Auth")
	EPlayServExternalAuthType Type = EPlayServExternalAuthType::None;

	/** The credential the game obtained, such as an Epic access token. */
	UPROPERTY(BlueprintReadWrite, Category="PlayServ|Auth")
	FString Token;

	/**
	 * Optional player-visible name. It names the player only when this login creates the player or first links this
	 * provider; a later login never renames anyone. Trimmed and cut to 64 characters, never refused; left out when empty.
	 */
	UPROPERTY(BlueprintReadWrite, Category="PlayServ|Auth")
	FString DisplayName;

	/** Optional extras for a method that carries more than one value. */
	UPROPERTY(BlueprintReadWrite, Category="PlayServ|Auth")
	TMap<FString, FString> AdditionalData;
};

/**
 * Authentication module. Call it through the PlayServ::Auth namespace (PlayServ.h), which checks the subsystem, or
 * directly through UPlayServSubsystem::Get()->GetAuth() when you already hold the subsystem.
 */
UCLASS()
class PLAYSERVRUNTIME_API UPlayServAuth : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Create a new guest player and sign them in. Every call creates a new player; to resume the same guest later,
	 * keep the refresh token (SetRefreshTokenChangedHandler) and sign in with LoginWithRefreshToken.
	 */
	void LoginAnonymous(FPlayServAuthCallback Callback);

	/**
	 * The same, naming the new player. The name is trimmed and cut to 64 characters, never refused, and left out when
	 * nothing remains. It cannot be changed by a later login.
	 */
	void LoginAnonymous(const FString& DisplayName, FPlayServAuthCallback Callback);

	/**
	 * Sign in with a credential from an external provider (Epic, Steam) that the game has obtained. A credential of type
	 * None fails at once, with no request.
	 */
	void LoginExternal(const FPlayServExternalCredential& Credential, FPlayServAuthCallback Callback);

	/**
	 * When the Epic Games Launcher started this process with an exchange code, fill OutCredential with an
	 * EpicLauncherExchangeCode credential for LoginExternal and return true; otherwise return false and leave it
	 * untouched. The game decides which path to take: the launcher path and the native EOS path lead to different
	 * platform identities. Callable before login. The code is never logged.
	 */
	static bool TryGetLauncherCredential(FPlayServExternalCredential& OutCredential);

	/**
	 * Hex-encode a Steam web-API ticket, as Steamworks returns it in GetTicketForWebApiResponse_t, into a
	 * SteamWebApiTicket credential. Returns false, leaving OutCredential untouched, for an empty ticket or one longer
	 * than Steam mints (2560 bytes). A game on OnlineSubsystemSteam already holds a hex string and sets Token itself.
	 */
	static bool TryMakeSteamCredential(TConstArrayView<uint8> TicketBytes, FPlayServExternalCredential& OutCredential);

	/**
	 * Resume the player of an earlier session with the refresh token kept from it. The SDK stores nothing on disk:
	 * keep the token that SetRefreshTokenChangedHandler hands you at login and on every rotation.
	 *
	 * The token is single-use and rotates about every 12 minutes of play; presenting a spent one makes the platform
	 * revoke the whole session, so never pass a token another live session is using. On failure, Error.Code decides
	 * what to do with the stored copy: NetworkUnreachable or Timeout mean the platform never saw it, so keep it and try
	 * again later; any other failure means the platform refused it, so discard it. An empty token, and a call while
	 * another refresh-token exchange is running, fail at once with no request.
	 */
	void LoginWithRefreshToken(const FString& InRefreshToken, FPlayServAuthCallback Callback);

	/**
	 * Start a server session with a server credential: the studio's sk_* server key, or the deployment token a server
	 * started by PlayServ hosting receives. No request is made; the session has no player, never expires and never
	 * fires OnSessionLost. Any other credential, a pk_* client key above all, is refused. Never ship a server key in a
	 * client build.
	 */
	void LoginServer(const FString& ApiKey, FPlayServSimpleCallback Callback);

	/**
	 * End the current session. Subscriptions end and the token refresh stops, and the callback always reports success.
	 * Revoke, the default, also signs a client session out on the platform; Local leaves its refresh token valid. See
	 * EPlayServLogoutMode.
	 */
	void Logout(FPlayServSimpleCallback Callback, EPlayServLogoutMode Mode = EPlayServLogoutMode::Revoke);

	/** The signed-in player's id; empty with no session or a server session. */
	UFUNCTION(BlueprintPure, Category="PlayServ|Auth")
	const FString& GetPlayerId() const;

	/** The session's access token; empty with no session. A credential: never show or log it. Not exposed to Blueprint. */
	const FString& GetAccessToken() const;

	/** True while a client or server session exists. */
	UFUNCTION(BlueprintPure, Category="PlayServ|Auth")
	bool IsLoggedIn() const;

	/** True while the session is a server session. */
	UFUNCTION(BlueprintPure, Category="PlayServ|Auth")
	bool IsServerSession() const;

	/** The current session type. */
	UFUNCTION(BlueprintPure, Category="PlayServ|Auth")
	EPlayServSessionType GetSessionType() const;

	/**
	 * Broadcast when the platform refuses the session's refresh token and the session is cleared: erase a stored copy
	 * here. Not broadcast when a refresh never reached the platform; those are retried and the session is kept.
	 */
	UPROPERTY(BlueprintAssignable, Category="PlayServ|Auth")
	FPlayServOnSessionLost OnSessionLost;

	/**
	 * Install the handler that receives the refresh token whenever it changes: at login and on every rotation, about
	 * every 12 minutes of play. Store it synchronously in the handler: the platform has already rotated the token when
	 * the handler runs, so a process that dies before the write leaves a spent token behind. Never called for a server
	 * session, on logout or on session loss; clear your stored copy from Logout's callback and from OnSessionLost. One
	 * handler: a second call replaces it, and an unbound delegate removes it.
	 */
	void SetRefreshTokenChangedHandler(FPlayServRefreshTokenChanged Handler);

	/**
	 * Whether a failed refresh means the token is spent, so the session is over and a stored copy should go. Not
	 * terminal: no answer (NetworkUnreachable, Timeout), and an answer from the platform's front end for an app that was
	 * not there to take the request or asked the client to slow down (HTTP 429, 502, 503, 504). The token is then still
	 * the live one: keep it and present it again later. Any other answer is terminal, a 500 among them: the app answered,
	 * and may have spent the token.
	 */
	static bool IsTerminalRefreshFailure(const FPlayServError& Error);

private:
	friend class UPlayServSubsystem;

#if !UE_BUILD_SHIPPING
	friend class FPlayServAuthTestAccess;
#endif

	void Init(TSharedPtr<class FPlayServHttp> InHttp);
	void Shutdown();

	// Warning: Logout runs this in both modes; a Local logout revokes nothing, so without it subscriptions outlive the session.
	FSimpleDelegate OnLogoutEndsRealtime;

private:
	void StartSession(const struct FPlayServAuthResponse& AuthResponse, EPlayServSessionType Type);
	void RefreshTokens(const struct FPlayServAuthResponse& AuthResponse);
	void ClearSession();
	void StartRefreshTimer();
	void StopRefreshTimer();

	enum class ERefreshExchange : uint8
	{
		Rotate,
		Login
	};

	void ExchangeRefreshToken(const FString& TokenToPresent, ERefreshExchange Mode, FPlayServAuthCallback Callback);

	void RefreshSession();

	void StartRefreshRetryTimer();

	static TSharedPtr<FJsonObject> BuildV2LoginBody(const FPlayServExternalCredential& Credential);

	static TSharedPtr<FJsonObject> BuildAnonLoginBody(const FString& DisplayName);

	static FString SanitizeDisplayName(const FString& DisplayName);

	static bool ParseLauncherExchangeCode(const TCHAR* CommandLine, FString& OutExchangeCode);

	static bool IsWebApiTicketHex(const FString& Ticket);

	static bool IsJwtShaped(const FString& Credential);

	// Warning: the signature is not verified; nothing may be trusted on the strength of this claim.
	static bool TryParsePlayerIdFromAccessToken(const FString& InAccessToken, FString& OutPlayerId);

	// Warning: executes the handler with a copy of the token, because a handler may call Logout, which empties the member.
	void NotifyRefreshTokenChanged() const;

	bool OnRefreshTick(float DeltaTime);

	TSharedPtr<class FPlayServHttp> Http;

	FString AccessToken;
	FString RefreshToken;
	FString PlayerId;
	int32 AccessTokenTTL = 0;
	int32 RefreshTokenLifetime = 0;
	EPlayServSessionType SessionType = EPlayServSessionType::None;

	// Warning: one guard for both refresh-token callers; two exchanges in flight present the same token and the platform revokes the session.
	bool bRefreshInFlight = false;

	int32 RefreshRetryCount = 0;

	FPlayServRefreshTokenChanged RefreshTokenChangedHandler;

	bool bRefreshTokenHandlerEverInstalled = false;

	FTSTicker::FDelegateHandle RefreshTickerHandle;

	// Warning: every refresh callback compares this before it touches the session; a response from an earlier session is dropped.
	uint32 SessionGeneration = 0;
};
