# Authentication

Include `PlayServ.h`. Every auth call is in the `PlayServ::Auth::` namespace.

## Login paths

| Path | Function | Notes |
|------|----------|-------|
| External provider (production) | `LoginExternal(Credential, Cb)` | Your game supplies a provider credential (an Epic access token, the Epic Games Launcher exchange code, or a Steam web-API ticket) and the SDK exchanges it for a player session |
| Epic Games Launcher entry | `TryGetLauncherCredential(OutCred)` | Not a login: finds the launcher's one-time exchange code on the command line and builds the credential you pass to `LoginExternal`. No request |
| Steam ticket bytes | `TryMakeSteamCredential(Bytes, OutCred)` | Not a login: hex-encodes a Steamworks web-API ticket into a credential for `LoginExternal`. No request |
| Anonymous (guest) | `LoginAnonymous(Cb)` / `LoginAnonymous(Name, Cb)` | Creates a new guest player (`plr_*`) on every call. The second form also names that player |
| Stored session (guest continuity) | `LoginWithRefreshToken(Token, Cb)` | Resumes the same player from a refresh token your game kept from an earlier session. Pair it with `SetRefreshTokenChangedHandler` |
| Server (dedicated) | `LoginServer(Cb)` / `LoginServer(ServerKey, Cb)` | The `sk_*` key is the credential: no request, no expiry, no player id. Never ship the key in a client build |

Every login callback is an `FPlayServAuthCallback`: `(bool bSuccess, const FString& PlayerId, const FPlayServError& Error)`. Server login uses `FPlayServSimpleCallback` (`bool`, `FPlayServError`), because a server session has no player id.

## External login

`LoginExternal` is the production login. The SDK does not talk to any OnlineSubsystem: your game obtains the provider credential and passes it in, and the platform verifies it with the provider and returns a session.

```cpp
void LoginExternal(const FPlayServExternalCredential& Credential, FPlayServAuthCallback Callback);
```

**The credential type, `EPlayServExternalAuthType`:**

| Value | What you provide | Platform identity |
|-------|------------------|-------------------|
| `EpicAccessToken` | An Epic Account Services access token that your game obtains (below) | Product User ID |
| `EpicLauncherExchangeCode` | The Epic Games Launcher's one-time exchange code, which `TryGetLauncherCredential` gets for you (below) | Epic Account ID |
| `SteamWebApiTicket` | A hex-encoded Steam web-API ticket; *Steam login* below says which Steamworks call mints a usable one | 64-bit SteamID |

> The two Epic values lead to different identities. A player who signs in through the launcher and a player who signs in with a native EOS token are different players on the platform, so pick one path per title and keep to it.

> A credential of type `None`, an empty token, or a Steam ticket that is not hex fails at once, with no request.

**The credential struct, `FPlayServExternalCredential`:**

```cpp
USTRUCT(BlueprintType)
struct FPlayServExternalCredential
{
    EPlayServExternalAuthType  Type;            // which credential Token carries
    FString                    Token;           // the credential you obtained
    FString                    DisplayName;     // optional: the new player's display name
    TMap<FString, FString>     AdditionalData;  // reserved: not sent
};
```

`DisplayName` is sent as the player's display name when you set it, for every credential type. It
counts only where the login creates the player or first links this provider: a later login never
renames a returning player. The SDK trims it and cuts it to 64 characters rather than refusing it,
without splitting a character, and leaves it out when it is empty.

`AdditionalData` is not sent on any login. It is reserved for a credential type that carries more
than one value.

For `EpicAccessToken`, only `Token` is required:

```cpp
FPlayServExternalCredential Cred;
Cred.Type  = EPlayServExternalAuthType::EpicAccessToken;
Cred.Token = EpicAccessToken;  // see "Obtaining the Epic access token" below

PlayServ::Auth::LoginExternal(Cred,
    FPlayServAuthCallback::CreateLambda(
        [](bool bOk, const FString& PlayerId, const FPlayServError& Err)
        {
            if (!bOk) { /* Err.Code, Err.ProblemCode, Err.Message */ return; }
            // logged in: PlayerId is the PlayServ player id (plr_*)
        }));
```

A login never carries an existing session, so `LoginExternal` during a guest session signs in as the provider's player; a failed login leaves the existing session as it was.

## Obtaining the Epic access token

The token comes from your EOS `OnlineSubsystem` after the player has signed in to Epic. How you read it depends on the EOS plugin you use (your project depends on `OnlineSubsystem`; the SDK does not):

- **Epic's `OnlineSubsystemEOS`**: `GetAuthToken(0)` returns the Epic Account Services access token:

  ```cpp
  IOnlineSubsystem*  OSS      = IOnlineSubsystem::Get(TEXT("EOS"));
  IOnlineIdentityPtr Identity = OSS->GetIdentityInterface();
  FString EpicAccessToken     = Identity->GetAuthToken(0);  // after the user is logged in
  ```

- **Redpoint EOS Online Subsystem**: `GetAuthToken(0)` returns another kind of token, which the platform refuses. Read the access token from the user account's auth attributes instead:

  ```cpp
  IOnlineSubsystem*  OSS      = IOnlineSubsystem::Get(TEXT("RedpointEOS"));
  IOnlineIdentityPtr Identity = OSS->GetIdentityInterface();
  FString EpicAccessToken;
  Identity->GetUserAccount(*Identity->GetUniquePlayerId(0))
          ->GetAuthAttribute(TEXT("epic.accessToken"), EpicAccessToken);
  ```

It must be the Epic access token, not an id token. Either way it goes into `Cred.Token`, and the SDK sends it as it is.

## Launching from the Epic Games Launcher

A title on the Epic Games Store is started by the launcher with a one-time exchange code on its command line instead of an access token. `TryGetLauncherCredential` finds it and builds the credential:

```cpp
bool TryGetLauncherCredential(FPlayServExternalCredential& OutCredential);
```

```cpp
FPlayServExternalCredential Cred;
if (PlayServ::Auth::TryGetLauncherCredential(Cred))
{
    // Started from the Epic Games Launcher: sign in with the exchange code.
    PlayServ::Auth::LoginExternal(Cred, Callback);
}
else
{
    // Started any other way: your own EOS sign-in and the EpicAccessToken path above.
}
```

It returns `false`, leaving `OutCredential` untouched, when the launcher did not start the process, so it is safe to call at every startup. It needs no SDK session and sends nothing. It needs both `-AUTH_TYPE=exchangecode` and a non-empty `-AUTH_PASSWORD` on the command line; `-EpicPortal` alone is not a code.

**Your game makes the choice.** `LoginExternal` never looks for a launcher code by itself: the launcher and native paths lead to different players (table above), and picking one from how the process happened to start would change which player signs in.

**The code works once.** The platform redeems it at once. If the login fails, the same code cannot be tried again; the player has to start the game from the launcher again. Treat a failed launcher login as "restart required", not "retry".

**Handle it like a password.** The SDK never logs it, only that it found one. It is on your process command line, though, where the engine's logging and the process list can show it, so do not copy the command line into your own logs or crash reports.

## Steam login

Steam goes through the same `LoginExternal`, with `Type = SteamWebApiTicket`. The platform verifies the ticket with Valve's `ISteamUserAuth/AuthenticateUserTicket`; the identity is the 64-bit SteamID.

```cpp
bool TryMakeSteamCredential(TConstArrayView<uint8> TicketBytes, FPlayServExternalCredential& OutCredential);
```

```cpp
// Steamworks: inside your GetTicketForWebApiResponse_t callback
FPlayServExternalCredential Cred;
if (PlayServ::Auth::TryMakeSteamCredential(
        MakeArrayView(Response.m_rgubTicket, Response.m_cubTicket), Cred))
{
    PlayServ::Auth::LoginExternal(Cred, Callback);
}
```

Steamworks hands the ticket over as bytes, and Valve's web API takes hex. `TryMakeSteamCredential` returns `false` for an empty ticket or one longer than Steam mints (2560 bytes). If you already have the ticket as a hex string, set `Type` and `Token` yourself. `LoginExternal` refuses a Steam token that is not hex before sending anything.

**Which Steam call mints a usable ticket.** The ticket must come from `ISteamUser::GetAuthTicketForWebApi` with no identity string (`GetAuthTicketForWebApi(nullptr)`): the platform verifies tickets without an identity, and Valve binds a ticket to the identity it was minted with. None of the wrong routes gives a local error:

| Route | What it mints | Usable |
|-------|---------------|--------|
| `ISteamUser::GetAuthTicketForWebApi(nullptr)` (Steamworks) | web-API ticket, unbound | ✓ |
| `OnlineSubsystemSteam` `GetAuthToken(0)` | session ticket (`GetAuthSessionTicket`) | ✗ |
| `GetLinkedAccountAuthToken(..., "Session")` | session ticket | ✗ |
| `GetLinkedAccountAuthToken(..., "")`, the default | encrypted app ticket | ✗ |
| `GetLinkedAccountAuthToken(..., "WebAPI")` | web-API ticket bound to an identity; `OnlineSubsystemSteam` mints none without one | ✗ |

A session ticket and a web-API ticket are both hex and look the same, so neither the SDK nor the platform can tell them apart: a wrong one shows only as a refused login. Until the platform accepts an identity, call Steamworks directly.

## Anonymous login

```cpp
PlayServ::Auth::LoginAnonymous(
    FPlayServAuthCallback::CreateLambda(
        [](bool bOk, const FString& PlayerId, const FPlayServError& Err)
        {
            // a new plr_* guest on success
        }));
```

Every call creates a new guest player with a full session: the access token renews itself and the refresh token rotates. Nothing identifies a returning guest by itself; to bring a guest back to the same player on the next launch, keep the session's refresh token and resume with `LoginWithRefreshToken` (next section). Anonymous login suits tests, PIE sessions and guest play.

Without a name, a guest shows as blank wherever the platform lists players, the operator console
included. The second overload names the player it creates:

```cpp
PlayServ::Auth::LoginAnonymous(EnteredNickname,
    FPlayServAuthCallback::CreateLambda(
        [](bool bOk, const FString& PlayerId, const FPlayServError& Err)
        {
            // a new plr_* guest, named EnteredNickname
        }));
```

The name is trimmed and cut to 64 characters, as `FPlayServExternalCredential::DisplayName` is, and
left out when nothing remains, so a nickname of spaces counts as none. It names the player this
call creates; a later login cannot change it.

## Session continuity: resuming the same guest

A guest comes back as the same player only if your game kept the session's refresh token and hands it back. The SDK writes nothing to disk: where a credential may be stored differs by platform and title (Windows Credential Manager, a keychain, a console's save system), and the plugin has no platform-specific code. The SDK gives you a hook and a login; your game supplies the storage.

```cpp
// 1. Install the hook once, before you log in. It is called at login and on every rotation.
PlayServ::Auth::SetRefreshTokenChangedHandler(
    FPlayServRefreshTokenChanged::CreateUObject(this, &UMyGameInstance::PersistRefreshToken));

void UMyGameInstance::PersistRefreshToken(const FString& RefreshToken)
{
    // Store it synchronously, before anything else (see below).
    MyCredentialStore.Save(TEXT("playserv.session"), RefreshToken);
}

// 2. Next launch: resume when you have a token, otherwise start a new guest.
FString Stored;
if (MyCredentialStore.Load(TEXT("playserv.session"), Stored))
{
    PlayServ::Auth::LoginWithRefreshToken(Stored,
        FPlayServAuthCallback::CreateLambda(
            [this](bool bOk, const FString& PlayerId, const FPlayServError& Err)
            {
                if (bOk) { /* the same plr_* as last time */ return; }

                // Never retry the same token, but keep it when the platform never saw it.
                if (Err.Code == EPlayServErrorCode::NetworkUnreachable
                    || Err.Code == EPlayServErrorCode::Timeout)
                {
                    // Offline: keep the entry and tell the player to check their connection.
                    return;
                }

                // Refused: the stored token is spent. Drop it and start a new guest.
                MyCredentialStore.Erase(TEXT("playserv.session"));
                PlayServ::Auth::LoginAnonymous(/* ... */);
            }));
}
else
{
    PlayServ::Auth::LoginAnonymous(/* ... */);
}
```

`LoginWithRefreshToken` is a login: it takes a credential and makes a session, whatever came before. It works for a session from any login path, though in practice only a guest needs it; a provider login finds the player by the provider identity.

> **Store the token on every rotation, not only at login.** The refresh token rotates about every 12 minutes of play. A copy kept from login is spent within the hour, so the next launch presents it, is refused, and the player becomes a new guest without a word. A process that dies between a rotation and your write leaves a spent token behind, which is why the handler should store it synchronously, first thing.

> **The token is single-use.** Presenting a spent token makes the platform revoke the whole session family, the live session included. Never pass `LoginWithRefreshToken` a token that another running session is using, and never retry one the platform refused. The SDK refuses a `LoginWithRefreshToken` made while another refresh-token exchange is running, its own automatic rotation included, and fails the call without sending it. Guard your login button too, so a double click does not race itself.

> **`NetworkUnreachable` means nothing reached the platform.** The SDK also reports it when it
> refuses to send, such as a `LoginWithRefreshToken` made while another refresh-token exchange is
> running. What the code tells you is that your credential was never presented, so your stored copy
> is still good.

> **`OnSessionLost` means the platform refused the session.** It is broadcast only when the platform answered and rejected the session's refresh token, which ends the whole family on the platform. A refresh that did not reach the platform broadcasts nothing: the token is untouched, and the SDK keeps the session and retries with backoff. Erasing your stored copy on a lost connection turns a Wi-Fi blip into a player who loses their guest profile for good.

> **Clear your storage on a revoking logout and on session loss, and nowhere else.** The rotation hook is not called for either, because it would then also be called at an ordinary shutdown and erase the token you need next launch. Erase your entry in the callback of a `Logout` in `Revoke` mode and in an `OnSessionLost` handler, where the token is spent. After `Logout(…, EPlayServLogoutMode::Local)` the token is still valid; keep it, since it is how that player comes back. On a failed `LoginWithRefreshToken`, check `Error.Code` first (the example above): only a refusal means the token should go.

The handler is a plain delegate and cannot be assigned from Blueprint, for the same reason `GetAccessToken` is not exposed: a refresh token in a Blueprint graph is one print node away from a log file. One handler is installed at a time; a second call replaces it, and a default `FPlayServRefreshTokenChanged` clears it. It is never called for a server session, which has no rotating credential.

The `FString` you receive is a copy, so it stays valid for the whole call even if your handler calls
back into the SDK, for instance `Logout` because your own write failed.

> **Keep the handler alive as long as the session.** The refresh belongs to an engine subsystem that
> lives as long as the process, so it keeps rotating after a shorter-lived object you bound has gone.
> Each rotation the handler misses spends the token in your storage, and the next launch presents a
> spent one, which revokes the family. Bind to something that outlives your levels, or `Logout` when
> the bound object goes away. The SDK logs a warning when a rotation finds the installed handler
> unbound.

Choose the storage with care. On Windows, `FPlatformMisc::SetStoredValue` is not a credential store: it writes plain text to the registry under `HKCU\Software\...`, and a plain-text ini on Mac and Linux. Use the platform's credential store (Windows Credential Manager through `CredWriteW`/`CredReadW`, the macOS/iOS keychain, a console's encrypted save).

## Server login (dedicated servers)

`LoginServer` starts a server session for a dedicated server, or another trusted process of yours, with a server credential: the environment's server key (`sk_*`), or the deployment token (a platform-signed JWT) that a server PlayServ hosting started finds in its `PLAYSERV_DEPLOYMENT_TOKEN` environment variable. No request is made: the credential is sent on every request, so there is no player id, no refresh token and no renewal, and the callback runs before `LoginServer` returns. A dedicated server that uses Rooms ([Rooms](Rooms.md)) does not call `LoginServer`: `PlayServ::Rooms::StartHosting` picks the credential (the deployment token first, then the server key) and signs in.

```cpp
// The key from the settings (Config/DedicatedServerGame.ini, or the -PlayServServerKey= and
// PLAYSERV_SERVER_KEY overrides). Fails with a configuration error when none is set.
PlayServ::Auth::LoginServer(
    FPlayServSimpleCallback::CreateLambda(
        [](bool bOk, const FPlayServError& Err)
        {
            if (!bOk) { /* with no key configured, Err.Message names the three places to set one */ return; }
            // server session started: PlayServ::Auth::IsServerSession() is true
        }));

// A key you got yourself, from a secret manager or an orchestrator.
PlayServ::Auth::LoginServer(ServerKey, Callback);
```

A credential that is neither an `sk_*` key nor shaped like a JWT (three dot-separated parts) is refused before anything changes, a `pk_*` client key above all. A server session is not bound by the tables' client ACLs ([Schema](Schema.md)): it reads and writes tables closed to clients, and reads, updates and deletes any player's rows in a player-owned table. It does not create rows there; a player's row is created on the player's side ([Data](Data.md), `LoadPlayerOwned`).

> **Security:** a server key opens your project's data to whoever holds it. Never put it in `DefaultGame.ini`, the `.uplugin` or any file a client build ships, and never log it. Keep it in `Config/DedicatedServerGame.ini` (loaded by server processes, packaged into server builds only), or in an environment variable or on the command line of your server hosts. The SDK keeps it in memory only: never a `UPROPERTY`, never visible to Blueprint, never logged.

## Session state

```cpp
bool                      bLoggedIn = PlayServ::Auth::IsLoggedIn();
bool                      bServer   = PlayServ::Auth::IsServerSession();
EPlayServSessionType      Type      = PlayServ::Auth::GetSessionType(); // None | Client | Server
const FString&            PlayerId  = PlayServ::Auth::GetPlayerId();   // empty for a server session
```

## Logout

```cpp
// Revoke (the default): the session is signed out on the platform too.
PlayServ::Auth::Logout(
    FPlayServSimpleCallback::CreateLambda(
        [](bool bOk, const FPlayServError& Err) { /* cleared */ }));

// Local: end the session on this machine only; the refresh token stays valid.
PlayServ::Auth::Logout(
    FPlayServSimpleCallback::CreateLambda(
        [](bool bOk, const FPlayServError& Err) { /* cleared, still resumable */ }),
    EPlayServLogoutMode::Local);
```

Logout clears the local session first, ending every live update ([Realtime](Realtime.md)) and the token renewal, and its callback always reports success. What happens to the refresh token depends on the mode:

| `EPlayServLogoutMode` | Refresh token | Use it for |
|---|---|---|
| `Revoke` (default) | Revoked on the platform (`POST /auth/players/sign-out`), whatever happens on the network. The player's sessions on other devices stay. | A player who signed in with a provider (Epic, Steam): signing in again brings the same player back. |
| `Local` | Left valid for its lifetime; nothing is sent. `LoginWithRefreshToken` with it resumes the same player. | **Anonymous players.** Their refresh token is the only way back into the account (every anonymous login creates a new player), so `Revoke` ends the account for good. |

`Local` leaves a live credential on the device: whoever holds the stored token can resume the player. Keep it only where the player expects to be remembered. A server session has nothing to revoke, so both modes clear it locally.

## Token renewal

A client session renews its access token at 80% of its lifetime (the platform sets the lifetime) on an engine ticker, so renewal carries on through world teardown and PIE restarts. Each renewal rotates the refresh token; the SDK runs one renewal at a time and stores the new token before any callback runs. When the platform refuses a renewal, the SDK clears the session and broadcasts `OnSessionLost`.

`OnSessionLost` is a dynamic multicast delegate; bind a `UFUNCTION()`:

```cpp
// AMyGameMode.h
UFUNCTION()
void HandleSessionLost();

// AMyGameMode.cpp
PlayServ::Auth::OnSessionLost().AddDynamic(this, &AMyGameMode::HandleSessionLost);
```

A server session has nothing to renew: no timer, and `OnSessionLost` is never broadcast for it.

To hear about each rotation, which session continuity needs, install a `SetRefreshTokenChangedHandler` handler (*Session continuity* above).

## The access token

`UPlayServAuth::GetAccessToken()` is C++ only (`UPlayServSubsystem::Get()->GetAuth()`), and not exposed to Blueprint. The access token is a bearer credential: shown in a print node or a UI string, it lets anyone act as the player until it expires. Pass it only to your own HTTP code, never to UI, logs or anything a user sees.

---

Part of the PlayServ SDK documentation: [README](../README.md) lists every page.
