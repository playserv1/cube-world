# Serialization

## Supported UPROPERTY types

| Type | Serialized as |
|------|---------------|
| `FString` | JSON string |
| `bool` | JSON bool |
| `int32` | JSON number |
| `int64` | JSON number (see the precision note below) |
| `float` / `double` | JSON number |
| `enum class : uint8` (`UENUM`) | JSON string: the short value name (`"Gold"`, never `"ERank::Gold"`) |
| `USTRUCT` | JSON object (nested, recursive over its own UPROPERTYs) |
| `TArray<T>` | JSON array of the above |
| `TObjectPtr<T>` to an entity class | JSON string (record id), or `null` |
| `TObjectPtr<T>` to a non-entity class | skipped |

Engine structs are not special-cased: `FDateTime` is saved as `{ "Ticks": <int64> }`, its only reflected field, not as an ISO-8601 string. `FName`, `FText`, `TMap` and `TSet` are not supported: a whole-class entity may declare them only as `Transient` ([Entities](Entities.md)).

## Field naming

Field names keep the case you declared. The UPROPERTY's name is what is sent, what is stored, what the table's field must be called ([Schema](Schema.md)), and what comes back on reads.

```cpp
UPROPERTY() FString PlayerName;                                    // sent as "PlayerName"
UPROPERTY() int32   XP;                                            // sent as "XP"
UPROPERTY() bool    bActive;                                       // sent as "bActive"
FPlayServFilter::Where(TEXT("PlayerName")).EqualTo(TEXT("Alice")); // the UPROPERTY's exact name
```

Field names are case-sensitive everywhere: `FooBar`, `fooBar` and `foobar` are three different fields.

The SDK protects this from one engine detail. `FName` comparison ignores case, and outside the
editor an `FName` does not keep its own spelling: every spelling shares one name-table entry, and the
first one your game registered wins. If any code creates `FName("PLAYERNAME")` before a record is
written, asking a property for its name returns that spelling.

The SDK never asks. It keys every field (top-level entity fields, the members of a `USTRUCT` inside
an entity, and the members of cloud-function request and reply structs) by the name captured at
compile time, so what you declared is what is sent, whatever else your game has named. If a C# cloud
function reads the same records, keep its property names identical to the UPROPERTY declarations.

## Enum values

An enum field is written as the UENUM's short value name and read back from either the short or the qualified name. The platform enum declares the allowed values (create it with the UENUM's values, [Schema](Schema.md)), and the platform refuses any other value with `EPlayServErrorCode::ValidationFailed` (`validation_failed`), so add a new value on the platform before writing records that use it.

## int64 precision

`int64` is sent as a JSON number. JSON numbers are doubles, so values above 2^53 (about 9 × 10¹⁵) can lose precision. That is fine for most game data (XP, currency, scores); for the full 64-bit range, store the value as an `FString` yourself.

## Transient and EditorOnly properties

Properties marked `Transient` or `EditorOnly` are never saved, never overwritten by a `Reload`, and not in the schema manifest, on an entity and inside a `USTRUCT` alike.

```cpp
UPROPERTY(Transient) float ClientOnlyAnimTime;  // never serialized
```

## Reload semantics

`Reload`, `Populate`, `PopulateArray` and realtime pushes ([Realtime](Realtime.md)) all overwrite: the entity's persisted fields are reset to their class defaults and the platform's document is applied on top, so a field cleared on the platform comes back as its default, not as stale local state. Local edits you have not saved are lost. The fields a partial entity does not persist are left alone. Afterwards the `PlayServOnChanged` functions ([Entities](Entities.md)) of the fields whose values changed run, then the instance's `Subscribe` delegates.

---

Part of the PlayServ SDK documentation: [README](../README.md) lists every page.
