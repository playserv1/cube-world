# Data

## Loading, saving and deleting

All operations below are in the `PlayServ::Data::` namespace (include `PlayServ.h`). Data calls run on the client plane (the client key, plus the player's session once logged in) or on the server plane (a server session, [Authentication](Authentication.md)). Whether a call is allowed is decided by the table's ACL and ownership ([Entities](Entities.md), [Schema](Schema.md)): a shared, client-readable table answers even before login, a player-owned table answers a client only with the owning player's session (and a server for everything except creating a row), and a client-closed table needs the server plane. A denied call fails with `Forbidden`.

### Type-scoped operations

```cpp
// Create in memory (does not contact the platform). The entity has no id until it is saved.
UMyPlayer* P = PlayServ::Data::Create<UMyPlayer>();

// Load a single entity by its record id.
PlayServ::Data::Load<UMyPlayer>(TEXT("rec_..."),
    [](bool bOk, UMyPlayer* P, const FPlayServError& Err) { ... });

// The one row of a singleton class (Entities.md). Every call returns a new instance.
PlayServ::Data::LoadSingleton<UGameConfig>(
    [](bool bOk, UGameConfig* Config, const FPlayServError& Err) { ... });

// A player's row of a player-owned class (Entities.md). On a client pass the local player's controller;
// on a dedicated server, that connection's controller.
PlayServ::Data::LoadPlayerOwned<UPlayerProfile>(PlayerController,
    [](bool bOk, UPlayerProfile* Profile, bool bCreated, const FPlayServError& Err)
    {
        if (!bOk) { /* Err.Code NotFound: the player has no row yet, and this call did not create one */ return; }
        if (bCreated) { /* the player's first time */ }
    });

// Load a collection by filter (Filters.md). Field names are your UPROPERTY identifiers, case-sensitive.
// Results are complete: the SDK follows the platform's pages until every match is returned.
PlayServ::Data::LoadAll<UMyPlayer>(
    FPlayServFilter::Where(TEXT("Level")).GreaterThan(10),
    [](bool bOk, TArray<UMyPlayer*> Players, const FPlayServError& Err) { ... });

// Delete by record id (no instance required).
PlayServ::Data::DeleteById<UMyPlayer>(TEXT("rec_..."),
    FPlayServSimpleCallback::CreateLambda(
        [](bool bOk, const FPlayServError& Err) { ... }));

// Delete every record matching a filter. Deleting every record takes FPlayServFilter::All().
PlayServ::Data::DeleteAll<UMySession>(
    FPlayServFilter::Where(TEXT("LastHeartbeat")).LessThan(StaleCutoff),
    FPlayServDeleteAllCallback::CreateLambda(
        [](bool bOk, const FPlayServDeleteAllResult& Result, const FPlayServError& Err)
        {
            if (!bOk) { return; }
            UE_LOG(LogMyGame, Display, TEXT("Cleaned %d stale sessions"), Result.DeletedCount);
        }));
```

> Entities are created in the transient package and held weakly by the SDK. Keep a strong reference (a `UPROPERTY`, `TStrongObjectPtr`, or `Rename(nullptr, DesiredOuter)` onto an owner) for as long as you use one, or the garbage collector may take it between an operation and its callback.

`DeleteAll` runs a query and then deletes the matching records one by one; it is not atomic, and a row created during the sweep survives. `FPlayServDeleteAllResult` is a `USTRUCT` with `int32 DeletedCount`.

`Create`, `Load`, `LoadAll`, `DeleteById` and `DeleteAll` do not compile for a singleton, and `Create` does not compile for a player-owned class: the compiler names the call to use instead.

**`LoadSingleton`.** `Save` on the returned instance sends the fields you changed; `PreconditionFailed` means another writer changed the row after you read it, so `Reload`, apply your change again and `Save`. `Save` refuses an instance that no `LoadSingleton` or `Reload` produced, whose class defaults would otherwise overwrite the shared row; `Delete` and `Subscribe` refuse a singleton. Clients read a singleton when its table is open to client reads; servers read and write it.

**`LoadPlayerOwned`.** Who the player is comes from the session, never from what a client claims. On a client the controller must be the local player's, and the row is the signed-in player's. On a dedicated server hosting PlayServ rooms ([Rooms](Rooms.md)) it is the player PlayServ verified for that connection at `VerifyTicket` (`PlayServ::Rooms::GetPlayerId`); a connection that presented no ticket is refused. The overload that takes a player id serves flows with no controller, such as a save after the player left: on a client the id must be the signed-in player's, on a server one your game verified. A client creates its missing row when the table is open to client writes; two first loads at the same moment end with one row, and one of them reports `bCreated`. A dedicated server loads and saves player-owned rows but does not create them. Where clients may not write the table, create the row on the player's side: in a cloud function the client calls after login ([Cloud functions](CloudFunctions.md)), which writes as the calling player and sets `player_id` to that player. A row that carries the player in `player_id` but that the platform says another player owns is refused. `Save` and `BulkSave` refuse a player-owned instance with no id, such as one made with `NewObject`: get the row with `LoadPlayerOwned`.

### Finding your records

Record ids are minted by the platform, so a game cannot derive a record's id from something it already knows, such as a player id or a level name. These patterns cover the usual cases:

1. **One row per player: `LoadPlayerOwned`.** Mark the class `PlayServPlayerOwned` ([Entities](Entities.md)) and let `LoadPlayerOwned` find the row. A player-owned table with owner read also scopes `LoadAll<T>(FPlayServFilter::None())` to the signed-in player's rows.
2. **A natural key in a field.** For shared tables, keep the key as an ordinary field (`LevelName`, `ClanTag`), query with `Where(TEXT("LevelName")).EqualTo(...)`, and keep the returned record's id in memory for the session. Declare the field unique on the platform so it stays one row.
3. **A cloud function that creates the player's rows.** For rows the player owns but only servers write (starting values the client must not choose), a function the player calls after login creates each row as that player, with `player_id` set to the caller; from then on `LoadPlayerOwned` finds them on both the client and the dedicated server.
4. **One row for the project: `LoadSingleton`** ([Entities](Entities.md)).

### Instance operations

```cpp
// Save. A new entity is created on the platform (the response binds its id);
// an existing one sends only the top-level fields changed since the last load/save.
PlayServ::Data::Save(P, FPlayServSimpleCallback::CreateLambda(
    [](bool bOk, const FPlayServError& Err)
    {
        // PlayServ::Data::GetRecordId(P) is bound from here on
    }));

// Refresh from the platform: overwrites the persisted fields, then runs the changed fields'
// PlayServOnChanged functions and the Subscribe delegates, then this callback.
PlayServ::Data::Reload(P, FPlayServSimpleCallback::CreateLambda(
    [](bool bOk, const FPlayServError& Err) { ... }));

// Delete this entity's record.
PlayServ::Data::Delete(P, FPlayServSimpleCallback::CreateLambda(
    [](bool bOk, const FPlayServError& Err) { ... }));

// Read the record id the platform minted (empty before the first Save).
FString Id = PlayServ::Data::GetRecordId(P);
```

`Reload`, `Delete`, `Populate` and `PopulateArray` require an entity that already has an id (loaded, or saved once). A singleton needs none for `Reload`, and cannot be deleted.

### Change tracking

`Save` sends only what changed. On `Create`, `Load` or `Save` the SDK keeps a snapshot of the entity's serialized state. A later `Save` compares the current state with the snapshot and sends a merge patch of the changed top-level fields, each with its whole new value: a changed struct member or array element sends the whole field that holds it. An entity with no snapshot (created in memory) sends its whole document. A `Save` with nothing changed succeeds without a request.

`Save` saves only the entity you call it on. References are saved as ids and not followed, so save referenced entities yourself.

### Optimistic concurrency

The SDK remembers the version of every record it reads or writes (from a load, a `LoadAll` or `PopulateArray`, a create, or an update) and sends it back as `If-Match` on the next update or delete of that record. If someone else changed the record in between, the write fails with `EPlayServErrorCode::PreconditionFailed` (`ProblemCode` `precondition_failed`): `Reload`, re-apply your change, and save again. A live update ([Realtime](Realtime.md)) carries no version: when one changes your instance the SDK forgets the version it held, so the next write goes out without `If-Match` instead of being refused against the older one. A write with no known version is accepted by the platform as is (the last write wins).

### Order of operations on one instance

`Save`, `Reload`, `Delete` and `Populate` on one entity instance run one at a time, in the order you called them: each starts when the previous one completes. Call several without waiting and they all complete; a save is queued, never refused.

```cpp
// All three complete in order; the final persisted state reflects the last save.
Player->Currency = 100; PlayServ::Data::Save(Player, Cb);
Player->Currency = 150; PlayServ::Data::Save(Player, Cb);
PlayServ::Data::Reload(Player, Cb);   // runs after both saves, never mid-save
```

- **Coalescing.** Consecutive waiting operations of the same kind run as one, and every callback you passed still fires. The running operation never absorbs a later one.
- **Changes are read when a save runs.** A queued `Save` collects the changed fields when it runs, not when you called it.
- **Delete is final.** A successful `Delete` cancels every operation still queued behind it on that instance; their callbacks fire with a `NotFound` error. A later `Save` on the same instance starts a new queue and creates a new record with a new id.
- **Garbage collection.** The queue holds the entity weakly; if it is collected before its turn, the waiting operation completes with an error.
- **Bulk calls are not queued.** `BulkSave` and `BulkDelete` are not ordered with per-instance calls, so do not mix the two on one instance at the same time.

### Bulk operations

```cpp
// Save a batch: one create or update per entity (instances deduplicated). New entities get ids.
TArray<UObject*> Dirty = { Player, Clan, Item1, Item2 };
PlayServ::Data::BulkSave(Dirty,
    FPlayServSimpleCallback::CreateLambda([](bool bOk, const FPlayServError& Err) { ... }));

// Delete a batch: one delete per entity. An entity without an id is reported as a failure.
PlayServ::Data::BulkDelete(EntitiesToDelete,
    FPlayServSimpleCallback::CreateLambda([](bool bOk, const FPlayServError& Err) { ... }));
```

Partial failure: if any write in a `BulkSave` fails, the callback fires with `bOk == false` and the first error; the entities that did succeed keep their fresh snapshots and ids. `BulkDelete` behaves the same way: every entity is attempted, and the callback reports `bOk == false` if any delete failed or any entity had no id.

### Records as JSON

A record is a flat JSON document: the platform's system fields and your fields, all at the top level. You see it only through the raw record calls (below) or in platform logs:

```json
{ "id": "rec_ABC…", "created_at": "…", "updated_at": "…", "owner": "plr_…", "Name": "Eric", "Level": 20 }
```

### Raw record access

`UPlayServData` (`UPlayServSubsystem::Get()->GetData()`) also has the JSON calls the entity layer is built on:

```cpp
void Get(const FString& EntityType, const FString& Id, FPlayServJsonCallback Callback);
void GetAll(const FString& EntityType, const FPlayServFilter& Filter, FPlayServJsonCallback Callback);
void GetAll(const FString& EntityType, const TArray<FString>& Ids, FPlayServJsonCallback Callback);
void GetAll(const FString& EntityType, const TArray<FString>& Ids, const FPlayServFilter& Filter, FPlayServJsonCallback Callback);
void Delete(const FString& EntityType, const FString& Id, FPlayServSimpleCallback Callback);
void DeleteAll(const FString& EntityType, const FPlayServFilter& Filter, FPlayServDeleteAllCallback Callback);
void DeleteAll(const FString& EntityType, const TArray<FString>& Ids, FPlayServDeleteAllCallback Callback);
```

`EntityType` is the table name (the class name without its `U` prefix). `Get` delivers the record document; `GetAll` delivers `{"items": [ ...records... ]}` with every page already merged. The overloads with ids fetch a set of records in one query: the records come back in the order asked, an unknown id is skipped rather than failing the batch, a repeated id is answered once per occurrence, and a filter, when given, must also hold. `FPlayServJsonCallback` is `(bool, const TSharedPtr<FJsonObject>&, const FPlayServError&)`.

## References between entities

An entity can hold references to other entities:

```cpp
UCLASS(PlayServEntity)
class UMyPlayer : public UObject
{
    GENERATED_BODY()

    UPROPERTY() TObjectPtr<UMyClan>         Clan;        // single ref
    UPROPERTY() TArray<TObjectPtr<UMyItem>> Inventory;   // ordered list of refs
};
```

References are persisted as record ids (a single reference is a one-to-one relation, an array an ordered many-relation). When you `Load` a `UMyPlayer`, its `Clan` and `Inventory[i]` come back as stubs: instances with the id set and no data.

```cpp
// After Load:
PS->Clan;                              // not null, but Clan->ClanName is empty
PlayServ::Data::GetRecordId(PS->Clan); // has the real record id

// Populate a single stub:
PlayServ::Data::Populate(PS->Clan, FPlayServSimpleCallback::CreateLambda(
    [](bool bOk, const FPlayServError& Err) { ... }));

// Populate every stub in an array property with a single batched query:
PlayServ::Data::PopulateArray(PS, TEXT("Inventory"), FPlayServSimpleCallback::CreateLambda(
    [](bool bOk, const FPlayServError& Err) { ... }));
```

`Populate` is `Reload` under a clearer name: fetch by id, overwrite, snapshot. `PopulateArray` removes repeated ids first; a stub whose record no longer exists keeps its id, with its data unset. Stubs keep a load shallow: loading a player does not load their clan, whose members would load their inventories. Your code decides how deep to go.

**Deleting a referenced record** is refused by the platform while other records still link to it: the delete fails with `EPlayServErrorCode::Conflict` (`ProblemCode` `in_use`). Clear the references first.

**Non-entity object pointers** (a `TObjectPtr<UObject>` to a class that is not marked as an entity) are not persisted and are a build error on a whole-class entity unless the property is `Transient`.

References inside a `USTRUCT` (a part) are persisted as plain id strings. They come back as stubs you can `Populate` one by one (`PopulateArray` works on arrays of entity references, not on arrays of structs), and the platform does not treat them as relations, so `in_use` protection and relation-aware queries do not apply to them.

---

Part of the PlayServ SDK documentation: [README](../README.md) lists every page.
