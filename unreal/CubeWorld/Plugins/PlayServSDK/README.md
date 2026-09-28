# PlayServ SDK for Unreal Engine

The Unreal Engine plugin for PlayServ: player login, game data kept as C++ `UObject` classes, live
updates, cloud functions, and dedicated-server rooms. Beta; the version is in `PlayServSDK.uplugin` and
in `PLAYSERV_SDK_VERSION` (`Core/PlayServVersion.h`).

## Requirements

- Unreal Engine 5.8, launcher or source build.
- A PlayServ project, with its client key (`pk_…`) and, for a dedicated server, its server key
  (`sk_…`).

## Installation

1. Copy this folder to `<YourGame>/Plugins/PlayServSDK/`. The plugin must sit inside the game's own
   `Plugins/` folder: its code generator is found only there, not through `AdditionalPluginDirectories`.
2. Enable the plugin, and add `"PlayServRuntime"` to your module's dependencies in its `.Build.cs`.
3. Build. The build log shows a line starting `PlayServ codegen: N entity classes registered at compile
   time`; if it does not, the code generator did not load.

## Minimal setup

`Config/DefaultGame.ini`:

```ini
[/Script/PlayServRuntime.PlayServSettings]
BaseURL=https://<your PlayServ host>
ClientKey=pk_...
```

```cpp
#include "PlayServ.h"

UCLASS(PlayServEntity)
class UMyPlayer : public UObject
{
    GENERATED_BODY()

    UPROPERTY() FString Name;
    UPROPERTY() int32   Level = 1;
};

PlayServ::Auth::LoginAnonymous(FPlayServAuthCallback::CreateLambda(
    [](bool bOk, const FString& PlayerId, const FPlayServError& Err)
    {
        UMyPlayer* Player = PlayServ::Data::Create<UMyPlayer>();
        Player->Name = TEXT("Eric");
        PlayServ::Data::Save(Player, FPlayServSimpleCallback());
    }));
```

Each entity class needs a matching table on the platform; [Schema](Docs/Schema.md) has the mapping.

## The PlayServ CLI

The SDK runs in your game; the PlayServ CLI (`playserv`) puts your own code on the platform. You need
it for cloud functions and for PlayServ hosting. Get it from the admin console's **Download CLI**
(Windows, Linux, macOS), and sign it in with a server key of the environment you work in:

```bash
playserv login        # prompts for the server key (sk_…); the environment comes with the key
playserv functions deploy --slug buy-upgrade --kind cloud_function --src ./buy-upgrade
```

For PlayServ hosting, declare the room type (a `game_server` function), give it a room configuration,
and push your Linux server image. The machine pool that runs it is created in the admin console
(Rooms → Game servers → your game server → Hosting).

```bash
playserv functions declare arena --kind game_server --hosting-mode process-per-room
playserv rooms config set arena --capacity 16 --reservation-ttl 10 --lifetime 3600 --idle-timeout 300 --max-rooms 50
playserv image push --slug arena --src ./Build/LinuxServer --dockerfile ./Docker/Dockerfile --tag 1.0.0
```

`image push` builds with Docker (buildx) on your machine; `--roll` also moves the pool onto the new
image. A dedicated server you run yourself needs only a room type with a room configuration
(`playserv functions declare arena --kind game_server`, then `rooms config set`): no image, no pool.
[Cloud functions](Docs/CloudFunctions.md) and [Rooms](Docs/Rooms.md) have the details, and
`playserv --help` lists every command.

## Documentation

| Page | What it covers |
|------|----------------|
| [Getting started](Docs/GettingStarted.md) | Prerequisites, installation, a first login, the subsystem and its modules |
| [Configuration](Docs/Configuration.md) | `DefaultGame.ini`, the server key, the request timeout, command-line and environment overrides, the endpoints the SDK calls |
| [Authentication](Docs/Authentication.md) | Epic, Steam, anonymous and server login; resuming a guest; session state, logout, token refresh |
| [Entities](Docs/Entities.md) | Declaring entity classes; client-writable and partial entities; singletons; one row per player; per-field change functions; record ids |
| [Data](Docs/Data.md) | Create, load, save, delete and query; change tracking; concurrency; bulk operations; references between entities |
| [Serialization](Docs/Serialization.md) | Supported property types and their JSON form |
| [Filters](Docs/Filters.md) | `FPlayServFilter`: operators, nested paths, the bulk-delete guard |
| [Realtime](Docs/Realtime.md) | `Subscribe` / `Unsubscribe`: live updates |
| [Cloud functions](Docs/CloudFunctions.md) | Calling your deployed functions, typed and untyped |
| [Schema](Docs/Schema.md) | The platform tables your entity classes need, and what the SDK checks at startup |
| [Errors and logging](Docs/Errors.md) | `FPlayServError`, error codes, `LogPlayServ` |
| [Best practices](Docs/BestPractices.md) | Patterns for production code |
| [Versioning](Docs/Versioning.md) | Version macros, supported engines, beta status, support |
| [Rooms](Docs/Rooms.md) | Dedicated-server rooms: hosting, admitting players, joining, map changes |
| [Epic Online Services setup](Docs/EpicOnlineServices.md) | The EOS product and credentials an Epic login needs |
| [Upgrading](Docs/Upgrading.md) | What to change in your code when a release changes the API |

## Upgrading to this release

0.7 removes every call that took or set a record id, and adds singletons (`LoadSingleton`), one row
per player (`LoadPlayerOwned`) and per-field change functions (`PlayServOnChanged`). See
[Docs/Upgrading.md](Docs/Upgrading.md).

## Support

Contact your PlayServ representative. Include the SDK version (`PLAYSERV_SDK_VERSION`), the engine
version, and a log with `LogPlayServ` at `Verbose`. The SDK never logs keys or tokens; do not add
any to a report.
