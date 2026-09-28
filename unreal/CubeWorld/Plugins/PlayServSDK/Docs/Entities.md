# Entities

## Declaring an entity

A persistent entity is a `UObject` subclass marked with the **`PlayServEntity` class specifier**. The mark is resolved at compile time by the SDK's code generator, which runs inside Unreal Header Tool during your normal build:

```cpp
UCLASS(PlayServEntity)                    // UCLASS(meta=(PlayServEntity)) is equivalent
class UMyPlayer : public UObject
{
    GENERATED_BODY()

    UPROPERTY() FString PlayerName;
    UPROPERTY() int32   Level = 1;
    UPROPERTY() double  Experience = 0.0;
};
```

Nothing is added to your class: no base class, no members, no methods. The generator registers the class, builds its serializer field table, and describes it in a **schema manifest** (`PlayServManifest.json`). The matching table is created on the platform ([Schema](Schema.md)), named after the class without its `U` prefix: `UMyPlayer` → `MyPlayer`.

Entities are **C++ classes**. Blueprint-defined classes cannot be marked as entities in this release.

> Renaming an entity class means a new table on the platform; the old table and its rows stay in place; nothing is migrated. Settle entity names before a table holds data you need.

## Supported field types

| UPROPERTY type | Persisted as |
|----------------|--------------|
| `FString` | text |
| `bool` | boolean |
| `int32`, `int64` | integer (see the `int64` precision note in [Serialization](Serialization.md)) |
| `float`, `double` | number |
| `enum class : uint8` (`UENUM`) | enum, created on the platform with the UENUM's values ([Schema](Schema.md)) |
| `USTRUCT` | a **part** (embedded object) |
| `TArray<T>` of the above | array (a `TArray<USTRUCT>` is a list of parts; a `TArray` of primitives is a JSON column) |
| `TObjectPtr<T>` / `T*` to another entity class | relation, saved as the referenced record's id ([Data](Data.md)) |
| `TArray<TObjectPtr<T>>` of entity refs | ordered relation list |

**Unsupported:** soft, weak and lazy object pointers, `TMap`, `TSet`, `FName`, `FText`, byte arrays, delegates, and references to classes that are not entities. A `UCLASS(PlayServEntity)` class that declares a field of an unsupported type **fails the build** with an error naming the type and the remedies. To keep a field off the platform, mark it `UPROPERTY(Transient)`: `Transient` and `EditorOnly` fields are never saved ([Serialization](Serialization.md)).

## Client-writable entities

Whether game clients may read or write a table is the table's **ACL**, set on the platform where you create the table ([Schema](Schema.md)); a table without client write accepts writes only from the server plane (a dedicated server or a cloud function). Declare the intent in C++ as well when players' clients are meant to create, update and delete rows directly:

```cpp
UCLASS(PlayServEntity, PlayServClientWritable)
class UMySettings : public UObject { ... };
```

The mark goes into the schema manifest, for tooling that builds the schema from your code. In this release you create the tables on the platform yourself ([Schema](Schema.md)), so set the table's client write there too. `UCLASS(PlayServClientWritable)` on a class that is not an entity is a build error.

## Partial entities

A class **without** `PlayServEntity` that marks one or more properties with `UPROPERTY(PlayServProperty)` is a **partial entity**: only the marked properties are persisted, everything else on the class is invisible to the SDK. Use it for a gameplay class that should persist a slice of itself.

```cpp
UCLASS()
class UMyCharacterState : public UObject
{
    GENERATED_BODY()

    UPROPERTY(PlayServProperty) int32 PersistedScore = 0;   // persisted
    UPROPERTY()                 float AnimTime = 0.f;       // not persisted
};
```

Rules enforced as build errors: a marked property may not be `Transient` or `EditorOnly`; a marked property may not be of an unsupported type; a whole-class entity may not carry `PlayServProperty` (the class already persists everything); `UPROPERTY(PlayServClientWritable)` requires `PlayServProperty` in a partial entity, may not sit on a `Transient` or `EditorOnly` property of a whole-class entity, and is an error on a class that is not an entity at all; `PlayServEntity` may only mark a concrete `UCLASS`, never an interface. Per-field client-write intent is recorded in the manifest only; the platform enforces access per table, not per field.

## Singletons: one row for the project

```cpp
UCLASS(PlayServSingleton)
class UGameConfig : public UObject
{
    GENERATED_BODY()

    UPROPERTY() FString MessageOfTheDay;
    UPROPERTY() int32   MaxPartySize = 4;
};
```

A singleton table holds one row per project and environment: game configuration, world state. `PlayServSingleton` makes the class an entity by itself. The row has no record id. Read it with `PlayServ::Data::LoadSingleton<T>` ([Data](Data.md)); the platform creates it with its field defaults the first time anyone reads it. Clients read a singleton; servers read and write it. Create the table as a singleton on the platform ([Schema](Schema.md)); the SDK warns at startup when a class and its table disagree. Build errors: `PlayServSingleton` together with `PlayServClientWritable` (on the class or a field) or with `PlayServPlayerOwned`, and a field that references a singleton class, which has no record id to reference.

## One row per player

```cpp
UCLASS(PlayServEntity, PlayServClientWritable, PlayServPlayerOwned)
class UPlayerProfile : public UObject
{
    GENERATED_BODY()

    UPROPERTY() FString Nickname;
    UPROPERTY() int32   Coins = 0;
};
```

`UCLASS(PlayServPlayerOwned)` gives the class one row per player, owned by that player on the platform: read by its owner, deleted with the player. Get a player's row with `PlayServ::Data::LoadPlayerOwned<T>` ([Data](Data.md)). The table keeps each row's player in a text field named `player_id`, unique and required ([Schema](Schema.md)); the SDK writes it when the row is created, and the class does not declare it. Build errors: `PlayServPlayerOwned` without `PlayServEntity`, and a persisted property named `player_id`.

## Reacting to changes: `PlayServOnChanged`

```cpp
UCLASS(PlayServEntity)
class UMyPlayer : public UObject
{
    GENERATED_BODY()

    UPROPERTY(PlayServOnChanged = OnCurrencyChanged) int32 Currency = 0;

    UFUNCTION() void OnCurrencyChanged(const int32& OldCurrency);
};
```

When the platform's state overwrites an instance that already holds data (a live update ([Realtime](Realtime.md)), a `Reload`, or a `Populate` of a loaded instance) and the field's value is different afterwards, the SDK calls the named function on the instance. The whole document is applied first; the functions run in field declaration order (the class's own fields, then its bases'); the `Subscribe` delegate runs after them, and a `Reload` callback last. The function may take the field's previous value, as its type or `const&` to it, or nothing. The previous value is what your instance held, which can be an unsaved edit the overwrite discarded.

It is not called for your own writes, a `Save`, a `Load`, or a value that did not change, and intermediate values can be skipped: the platform delivers the latest state, not every change. A change inside a `USTRUCT` or `TArray` field calls the field's function once. A function that writes a persisted field leaves it dirty for the next `Save`, so keep derived state in `Transient` fields. The function must be a `UFUNCTION` of the class or a base that returns `void` and is not static, an RPC or a Blueprint event; anything else fails the build, as does the mark on a field that is never persisted or inside a `USTRUCT`.

## Record identity

Record ids are minted by the platform (`rec_*`). An entity created in memory has an empty record id until its first `Save` completes; the save binds the returned id, and `PlayServ::Data::GetRecordId(Entity)` reads it from then on. There is no way to choose an id: `Create<T>()` takes none, and the SDK binds ids itself, from a save, a load or a reference it reads. How a game reaches a record whose id it does not know is the subject of *Finding your records* in [Data](Data.md). A singleton's row has no id: `GetRecordId` returns an empty string for it.

---

Part of the PlayServ SDK documentation: [README](../README.md) lists every page.
