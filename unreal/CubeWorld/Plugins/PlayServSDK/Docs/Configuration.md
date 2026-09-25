# Configuration

## DefaultGame.ini

The SDK reads the project's standard `Config/DefaultGame.ini` (`UPlayServSettings`). Add one section:

```ini
[/Script/PlayServRuntime.PlayServSettings]
BaseURL="https://<your-platform-host>"
ClientKey=pk_<env>_...
RoomDefaultSlug=<your-game_server-slug>
RequestTimeoutSeconds=20.0
```

- `BaseURL` — the PlayServ host for the environment this build targets. Every request, and the live-update connection, goes to it.
- `ClientKey` — the environment's **client key** (`pk_*`). It names your project and environment, and is safe to ship in a client build.
- `RoomDefaultSlug` — optional, and only if you use Rooms ([Rooms](Rooms.md)): your project's default **room type**, the slug of a `game_server` function. Your dedicated server registers its rooms under it, and the `Browse` and `JoinRoom` overloads without a slug use it on the client. It is not a secret and clients need it, so it goes here rather than in the server file. Override at launch with `-PlayServRoomDefaultSlug=` or `PLAYSERV_EXECUTOR_SLUG`.
- `RequestTimeoutSeconds` — optional, default `20.0` (see below).

> **Keep the URL quoted.** Unreal's ini parser treats `//` in an unquoted value as the start of a comment and truncates the URL to `https:`.

## DedicatedServerGame.ini: the server key

A dedicated server authenticates on the **server plane** with the environment's **server key** (`sk_*`, a secret). It has its own file, `Config/DedicatedServerGame.ini`:

```ini
[/Script/PlayServRuntime.PlayServSettings]
ServerKey=sk_<env>_...
```

The engine loads `Config/DedicatedServer<Type>.ini` only in a process that `IsRunningDedicatedServer()`, and UAT stages files with that prefix only into server builds, so a client package never contains it. Every other setting still comes from `DefaultGame.ini`; the server file adds only this key. The SDK logs an **error** if a server key is present in a process that is not a dedicated server.

> **Put only secrets here.** Anything a client also needs, `RoomDefaultSlug` in particular, must live in `DefaultGame.ini`, because a client process never loads this file and will read the value back empty.

## Request timeout

`RequestTimeoutSeconds` (default **20.0**) bounds every SDK request. A request that exceeds it fails with `EPlayServErrorCode::Timeout` ([Errors and logging](Errors.md)) instead of hanging on the engine's default HTTP timeout. Set it to `0` to fall back to the engine default. There is no launch override: a build ships with one timeout.

## Project Settings UI

The settings also appear under **Project Settings → Game → PlayServ**. Edits made in-editor write back to `DefaultGame.ini`; `ServerKey` is shown as a password field.

## Command-line and environment overrides

The connection values can be overridden at launch, for CI, staging or testing against several environments. Overrides apply when the SDK starts; keys are never logged, the base URL is.

```
UnrealEditor.exe YourGame.uproject -game ^
    -PlayServBaseURL="https://staging.your-platform-host" ^
    -PlayServClientKey=pk_...

UnrealEditor.exe YourGame.uproject YourMap -server ^
    -PlayServBaseURL="https://staging.your-platform-host" ^
    -PlayServServerKey=sk_...
```

| Switch | Overrides |
|--------|-----------|
| `-PlayServBaseURL=<url>` | `BaseURL` (also honours `PLAYSERV_API_URL`, which PlayServ hosting sets for a server it starts) |
| `-PlayServClientKey=<pk_...>` | `ClientKey` |
| `-PlayServServerKey=<sk_...>` | `ServerKey` (also honours the `PLAYSERV_SERVER_KEY` environment variable; precedence is command line > environment > ini) |
| `-PlayServRoomDefaultSlug=<slug>` | `RoomDefaultSlug` (also honours `PLAYSERV_EXECUTOR_SLUG`; see [Rooms](Rooms.md)) |

## Validation

At subsystem init the SDK logs a warning (`LogPlayServ`) if `BaseURL` or `ClientKey` is empty, and an error if a `ServerKey` is present in a non-server process. A request without a base URL, or a client request without a client key, fails at once with a configuration error, before anything is sent.

## Network endpoints the SDK uses

Everything goes to the host in `BaseURL`. Useful when configuring proxies, firewalls or log filters:

| Path | Used for |
|------|----------|
| `POST /auth/players/anon`, `/login`, `/refresh`, `/sign-out` | Player sessions ([Authentication](Authentication.md)) |
| `GET /data/tables`, `/data/tables/{table}/records...` | Entity persistence ([Data](Data.md)); one `GET /data/tables` per process resolves your entity classes to their platform tables |
| `wss://<host>/ws/` | Live updates ([Realtime](Realtime.md)); one WebSocket per process, opened on the first `Subscribe` |
| `POST /fn/{slug}` | Cloud functions ([Cloud functions](CloudFunctions.md)) |

Credentials: client-plane requests carry the client key in the `X-PlayServ-Client` header plus the player's bearer token once a player is logged in; server-plane requests carry the server key as the bearer. The SDK never mixes the two planes on one request.

---

Part of the PlayServ SDK documentation: [README](../README.md) lists every page.
