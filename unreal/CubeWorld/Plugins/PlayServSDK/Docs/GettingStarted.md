# Getting started

## Prerequisites

- **Unreal Engine 5.8**, a source or a launcher build. The SDK's compile-time code generator is a C# plugin for Unreal Header Tool; Unreal Build Tool compiles it with the engine's bundled .NET, nothing extra to install.
- **A PlayServ project** with at least one environment (for example `dev` and `prod`). From the PlayServ admin console you need:
  - the **base URL** of the platform host your build targets;
  - a **client key** (`pk_*`) for the environment, the public credential a game client ships with;
  - for dedicated servers, a **server key** (`sk_*`) for the environment, a secret.
  Keys are minted per project and environment on the project's **API keys** page; a server key's secret is shown once.
- **A table for each entity class** you persist, created on the platform, in the admin console or through the PlayServ MCP server ([Schema](Schema.md)).
- **For Epic login:** an EOS product configured for your title ([Epic Online Services setup](EpicOnlineServices.md)) and its Client ID, Client Secret and Deployment ID registered on the project's **Players → Auth providers → Epic Games** page of the admin console. Until that is done every Epic login answers `409 Provider 'epic' is not configured`.
- **For Steam login:** the Steam **Web API key** and **App ID** registered on the same page under Steam.
- **For cloud functions and PlayServ hosting:** the **PlayServ CLI** (`playserv`), from the admin console's *Download CLI* action; the [README](../README.md) shows the commands ([Cloud functions](CloudFunctions.md), [Rooms](Rooms.md)).
- **For PlayServ hosting:** Docker with buildx, which `playserv image push` builds your server image with.

## Installation

1. Copy the `PlayServSDK/` folder you received (these pages are inside it, under `Docs/`) into your project's `Plugins/` directory:
   ```
   <YourGame>/Plugins/PlayServSDK/
   ```
   > **The plugin must be inside your project's `Plugins/` folder.** Referenced through
   > `AdditionalPluginDirectories` it still builds without an error, but Unreal Build Tool does not
   > load its code generator, so `UCLASS(PlayServEntity)` classes are not entities at runtime. To keep
   > the SDK somewhere else (a shared checkout, a submodule), link it into `Plugins/`; on Windows a
   > directory junction needs no administrator rights:
   > ```
   > mklink /J <YourGame>\Plugins\PlayServSDK <path-to>\PlayServSDK
   > ```

2. Enable the plugin in your `.uproject`:
   ```json
   {
     "Plugins": [
       { "Name": "PlayServSDK", "Enabled": true }
     ]
   }
   ```
3. Add the runtime module to your game's `Build.cs`:
   ```cs
   PublicDependencyModuleNames.AddRange(new[] { "PlayServRuntime" });
   ```
4. Regenerate project files and build the editor target.

**Check the build.** A successful build logs one line from the code generator:

```
PlayServ codegen: N entity classes registered at compile time, M struct member tables, K enums in the manifest (manifest: ...)
```

If that line is missing, the code generator did not run, almost always because the plugin is not inside `Plugins/` (the note above). At runtime the first entity operation logs `PlayServ codegen registry: N entity class descriptor(s) active`.

The plugin has one Unreal module, `PlayServRuntime`, which your game links against, and the code generator under `Source/PlayServUht/`, which Unreal Build Tool compiles and loads into Unreal Header Tool by itself. There is nothing to enable or configure for it.

## Minimal usage

```cpp
#include "PlayServ.h"

// EpicAccessToken is the Epic access token your game obtained from its EOS
// OnlineSubsystem; Authentication.md shows how to get it.
void ExampleLogin(const FString& EpicAccessToken)
{
    FPlayServExternalCredential Cred;
    Cred.Type  = EPlayServExternalAuthType::EpicAccessToken;
    Cred.Token = EpicAccessToken;

    PlayServ::Auth::LoginExternal(Cred,
        FPlayServAuthCallback::CreateLambda(
            [](bool bOk, const FString& PlayerId, const FPlayServError& Err)
            {
                if (bOk)
                {
                    UE_LOG(LogTemp, Display, TEXT("Logged in as %s"), *PlayerId);
                }
            }));
}
```

## The subsystem

The SDK runs as `UPlayServSubsystem`, a `UEngineSubsystem`: one per process, alive across PIE sessions, with no world context needed.

```cpp
#include "PlayServ.h"

// The PlayServ namespace: checks that the SDK is running, then calls the module
PlayServ::Auth::LoginExternal(Credential, Callback);
PlayServ::Data::Save(Entity, Callback);
PlayServ::Code::Call(TEXT("my-function"), Params, Callback);

// The same calls on the modules themselves
UPlayServSubsystem* PS = UPlayServSubsystem::Get();
PS->GetAuth()->LoginExternal(Credential, Callback);
PS->GetData()->SaveEntity(Entity, Callback);
```

Both reach the same code. Use the namespace (`PlayServ.h`); the direct form suits code that already holds the subsystem and checks it for null itself. When the SDK is not running, such as in an editor utility, before startup or after shutdown, every namespace call completes with `FPlayServError::SubsystemUnavailable()`.

The subsystem's modules:

| Accessor | Type | Purpose |
|----------|------|---------|
| `GetAuth()` | `UPlayServAuth*` | Login, logout, session state, token refresh |
| `GetData()` | `UPlayServData*` | Entities: load, save, delete, query, live updates; records as JSON |
| `GetCode()` | `UPlayServCode*` | Cloud-function calls |
| `GetRooms()` | `UPlayServRooms*` | Rooms: hosting them on a dedicated server, joining them from a client ([Rooms](Rooms.md)) |

**Threading.** Every callback fires on the game thread.

**Blueprint.** The SDK is a C++ API. Blueprints get state queries and events only: the session queries (`IsLoggedIn`, `GetPlayerId`, `GetSessionType`, `IsServerSession`) and `OnSessionLost`, the Rooms queries and events ([Rooms](Rooms.md)), and the structs they carry. Entity classes are C++ classes ([Entities](Entities.md)).

---

Part of the PlayServ SDK documentation: [README](../README.md) lists every page.
