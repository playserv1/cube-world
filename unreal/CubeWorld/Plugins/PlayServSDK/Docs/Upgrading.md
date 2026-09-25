# Upgrading

One section per release that changes the API. Each lists what changed, what to do, and how you will
notice. The API is described in the pages of this folder, listed in the [README](../README.md).

## Upgrading from 0.6 to 0.7

Record ids are the platform's alone: every call that let you choose or pass one is gone. Two new
kinds of table replace the patterns those calls served (one row for the whole project, one row per
player), and a field can name a function the SDK calls when the platform changes it.

| 0.6 | 0.7 |
|-----|-----|
| `PlayServ::Data::GetEntityId(Entity)` | `PlayServ::Data::GetRecordId(Entity)` |
| `PlayServ::Data::SetEntityId(Entity, Id)` | removed: the SDK binds ids from a save, a load or a reference |
| `PlayServ::Data::Create<T>(Id)` | `PlayServ::Data::Create<T>()` |
| `PlayServ::Data::LoadOrCreate<T>(Id, Callback)` | a player's row: `PlayServ::Data::LoadPlayerOwned<T>(PlayerController, Callback)`; anything else: `Load<T>(Id, …)`, then `Create<T>()` on `NotFound` |
| a config table read with `LoadAll` and its first row | `UCLASS(PlayServSingleton)` and `PlayServ::Data::LoadSingleton<T>(Callback)` |
| a per-player table read with `LoadAll` on an owner-read table | `UCLASS(PlayServEntity, PlayServPlayerOwned)` and `LoadPlayerOwned` |

### Code

A player's profile, before:

```cpp
PlayServ::Data::LoadOrCreate<UPlayerProfile>(StoredProfileId,
    [](bool bOk, UPlayerProfile* Profile, bool bCreated, const FPlayServError& Err) { ... });
```

After: mark the class player-owned, and let the SDK find the player's row:

```cpp
UCLASS(PlayServEntity, PlayServClientWritable, PlayServPlayerOwned)
class UPlayerProfile : public UObject
{
    GENERATED_BODY()

    UPROPERTY() int32 Coins = 0;
};

PlayServ::Data::LoadPlayerOwned<UPlayerProfile>(PlayerController,
    [](bool bOk, UPlayerProfile* Profile, bool bCreated, const FPlayServError& Err) { ... });
```

No record id is stored anywhere. On a dedicated server, pass the connection's controller: the SDK
uses the player PlayServ verified for it, never an id the client sent.

### On the platform

- **Singletons.** Create the entity of a `PlayServSingleton` class as a singleton, closed to client
  writes, and give every required field a default.
- **Player-owned tables.** Create the table of a `PlayServPlayerOwned` class owned by the player,
  with a text field named `player_id`, `unique` and `required`, which your class does not declare.
  Adding that field to a table that already holds rows is a migration (propose, plan, apply), and
  every existing row needs its player's id first. Use `cascade-delete` as the player-delete policy.
- **Who creates a player's row.** A client's `LoadPlayerOwned` creates it in a table open to client
  writes. A dedicated server loads and saves player-owned rows but does not create them: for a table
  clients may not write, create the rows in a cloud function the client calls after login, writing
  as the calling player with `player_id` set to that player. From then on `LoadPlayerOwned` finds
  them on both sides.

### The plugin folder

The documentation moved from the single `SDK-API-Reference.md` to the pages under `Docs/`. Replace
`Plugins/PlayServSDK/` as a whole; copying 0.7 on top of 0.6 leaves the old file behind, describing
0.6.

### How you will notice

- Compile errors: `GetEntityId`, `SetEntityId`, `LoadOrCreate` and `Create<T>(Id)` no longer exist.
  `Create`, `Load`, `LoadAll`, `DeleteById` and `DeleteAll` on a singleton, and `Create` on a
  player-owned class, fail with a message naming the call to use.
- Build errors from the header tool for a wrong mark, each naming the class, the field and the fix.
- At startup, `LogPlayServ` warns when a class's `PlayServSingleton` marking and its table do not match.
- `Save` and `BulkSave` refuse a player-owned instance with no record id, such as one made with
  `NewObject`; get the row with `LoadPlayerOwned`.
- `Reload` now calls your `Subscribe` delegate for the fields it changed, and a reference to the same
  record is no longer reported as changed.
