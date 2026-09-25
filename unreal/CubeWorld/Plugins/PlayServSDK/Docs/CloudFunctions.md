# Cloud functions

Cloud functions are C# functions deployed to your PlayServ project, called from Unreal by slug. The SDK only calls them; you write and deploy them with the PlayServ CLI.

## Calling a function with USTRUCTs (recommended)

The typed overloads take a request `USTRUCT` and read the reply into a response `USTRUCT`. The reply is read strictly: a field of the wrong type fails the call with `ContractMismatch` instead of leaving a default behind.

```cpp
#include "PlayServ.h"

// USTRUCTs matching the function's C# request and reply types. Field names are sent as declared.
USTRUCT()
struct FBuyUpgradeRequest  { GENERATED_BODY()
    UPROPERTY() int32   UpgradeType = 0;
    UPROPERTY() bool    Premium = false;
};
USTRUCT()
struct FBuyUpgradeResponse { GENERATED_BODY()
    UPROPERTY() int64          Currency = 0;
    UPROPERTY() TArray<int32>  UpgradeLevels;
};

FBuyUpgradeRequest Req;
Req.UpgradeType = 1;
Req.Premium     = true;

// Call<TRequest, TResponse>: a typed request and reply.
PlayServ::Code::Call<FBuyUpgradeRequest, FBuyUpgradeResponse>(TEXT("buy-upgrade"), Req,
    [](bool bOk, const FBuyUpgradeResponse& Resp, const FPlayServError& Err)
    {
        if (!bOk) { /* Err.Code (ContractMismatch on drift), Err.ProblemCode, Err.Message */ return; }
        UE_LOG(LogMyGame, Display, TEXT("New currency: %lld"), Resp.Currency);
    });

// Call<TRequest>: a typed request; the reply is ignored.
PlayServ::Code::Call<FRecordEventRequest>(TEXT("record-event"), EventReq,
    [](bool bOk, const FPlayServError& Err) { /* ... */ });
```

A slug is lowercase letters, digits and dashes (`^[a-z][a-z0-9-]*$`) and is the whole address: the SDK posts to `/fn/<slug>`, with no sub-paths.

**How the conversion behaves** (`Code/PlayServRpcConverter.h`; unlike the entity serializer, it never coerces a value):

- The request does not serialize: `ContractMismatch`, and nothing is sent.
- The call fails (no session, an unknown slug, an error in the function): that error ([Errors and logging](Errors.md)).
- A reply field has the wrong JSON type: `ContractMismatch`, never a success with defaults.
- A missing reply field keeps its C++ default, and a missing or empty reply is a success with a default response, for functions that return nothing.

**Reply casing.** Reply fields are matched ignoring case, so a C# reply in camelCase (`{"currency": 70}`) fills `Currency`. A key that differs by more than case, such as `new_level` for `NewLevel`, fills nothing, and the field keeps its default without an error. Spell the C# reply properties like the UPROPERTYs, or give them attributes.

Supported property types in request and response structs: `FString`, `bool`, `int32`, `int64`, `float`, `double`, `UENUM` (by name), nested `USTRUCT`, `TArray` of those. Object pointers are skipped; `Transient` / `EditorOnly` fields are never sent.

## Calling a function with parameters built at runtime

When the request is not a fixed struct (keys decided at runtime, JSON passed through), use the untyped overload with `FPlayServRPCParams`. The result carries the reply as JSON.

```cpp
#include "PlayServ.h"
#include "Code/PlayServRPCParams.h"

// Param keys must match the C# request property names exactly (case-sensitive).
FPlayServRPCParams Params = FPlayServRPCParams()
    .Set(TEXT("UpgradeType"), 1)
    .Set(TEXT("Premium"),     true);

PlayServ::Code::Call(TEXT("buy-upgrade"), Params,
    FPlayServRPCCallback::CreateLambda(
        [](bool bOk, const FPlayServRPCResult& Result, const FPlayServError& Err)
        {
            if (!bOk) { /* Err.Code, Err.Message */ return; }
            // Result.Response is the function's reply as TSharedPtr<FJsonObject>.
            double Currency = Result.Response->GetNumberField(TEXT("Currency"));
        }));
```

`FPlayServRPCResult` is a `USTRUCT` with `TSharedPtr<FJsonObject> Response`. Empty params (`FPlayServRPCParams()` with no `.Set()`) send `{}`; an empty slug fails before anything is sent.

### FPlayServRPCParams.Set overloads

```cpp
Set(Key, FString)                          // JSON string
Set(Key, int32)                            // JSON number
Set(Key, int64)                            // JSON number (see the precision note in Serialization.md)
Set(Key, double)                           // JSON number
Set(Key, bool)                             // JSON bool
Set(Key, TSharedPtr<FJsonValue>)           // raw JSON value
Set(Key, TSharedPtr<FJsonObject>)          // nested object
```

The parameters are sent as one flat JSON object, keyed by the parameter names.

## Who is calling

A function declares in its manifest whether it needs a player (`auth: player`) or accepts any caller (`auth: none`). A function that needs a player, called without a client session, fails with `Unauthorized`. The caller's identity comes from the platform, never from the request body: do not put a player id in your request struct and expect the function to trust it. Inside the function, `FunctionRequest.ActingPlayerToken` carries the calling player's credential, and `Platform.Records.AsPlayer(token)` reads and writes records as that player: rows it creates in a player-owned table belong to the player, and owner-only reads apply. A call with no player session, or from a server, carries no acting player, so a function that needs one must refuse such calls itself.

## Errors

| Condition | `Error.Code` | `ProblemCode` |
|-----------|--------------|---------------|
| No function deployed under that slug | `NotFound` | `not_found` |
| Player-scoped function (`auth: player`) called without a player session | `Unauthorized` | `player_auth_required` |
| The function could not be reached, or took longer than 60 s | `Unknown` (HTTP 502) | `function_unreachable`; `Message` says `unreachable` or `timed_out` |
| The function threw, or returned an error status | `Unknown`, or the code for that 4xx status | whatever the function returned |
| The request struct does not serialize, or the reply does not convert | `ContractMismatch` | — |

## Writing and deploying functions

A function is one **directory**: a `platform.json` manifest (`name` = the slug, `kind: cloud_function`, `language: csharp`, `entry: "Namespace.Class"`, `auth: none|player`, optional `env{}`) plus C# sources implementing `PlatformSdk.IPlatformFunction` (parameterless constructor; the host creates one shared instance). You deploy source; the platform builds it. Deploy with the CLI:

```bash
playserv login                        # prompts for a server key (sk_…) of the target project and environment
playserv functions deploy --slug buy-upgrade --kind cloud_function --src ./buy-upgrade --env dev
```

Worth knowing:

- Manifest `env` keys starting with `PLATFORM_` are reserved and refused (`422 invalid_manifest`).
- Shared code comes in through a `ProjectReference` and is copied into the deploy bundle; a shared project may not carry its own `PackageReference`s or a `Program.cs`.
- Replies default to camelCase, which the typed overloads read (*Reply casing* above). Records written by a function are case-sensitive and refuse unknown fields (`422 unknown_field`), so its entity property names must match the UPROPERTYs exactly, case included.

`playserv init <slug>` scaffolds a function project, `playserv publish` deploys every function of a solution, and `playserv functions logs`, `rollback` and `secrets set` cover operations. `playserv --help` lists every command, and `playserv <command> --help` its options.

---

Part of the PlayServ SDK documentation: [README](../README.md) lists every page.
