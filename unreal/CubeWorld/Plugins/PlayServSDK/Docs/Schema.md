# Schema

Every entity class ([Entities](Entities.md)) needs a matching table on the platform, in each environment you run against, before the SDK can read or write it. The SDK knows the shape it needs: on every build the code generator writes each entity class, its fields and their kinds, and the parts and enums they use, into a schema manifest (`PlayServManifest.json`, under the plugin's `Intermediate/` folder). In this release you create and change the tables on the platform; the editor does not push them:

- **The PlayServ admin console**: the project's schema pages. Create an entity per class, a part per `USTRUCT` and an enum per `UENUM`, and set each table's ownership, read policy and client access.
- **The PlayServ MCP server**: the same operations through your AI assistant, `create_entity`, `create_part`, `create_enum`, and `propose_migration` → `plan_migration` → `apply_migration` for a change that would break existing rows. Point the assistant at your entity headers, or at the manifest, and have it follow the mapping below. Ask your PlayServ representative for your project's MCP connection.

## Naming

The platform addresses tables by name, and the SDK resolves your classes to them once per process (`GET /data/tables`):

| In C++ | On the platform |
|--------|-----------------|
| `UCLASS(PlayServEntity) class UMyPlayer` | entity `MyPlayer` (class name without the `U` prefix) |
| `USTRUCT() struct FPlayerStats` used by an entity field | part `PlayerStats` (struct name without the `F` prefix) |
| `UENUM() enum class ERank : uint8` | enum `ERank` (the `E` prefix stays), values by their short names (`Gold`, never `ERank::Gold`) |
| `UPROPERTY() int32 XP` | field `XP`: names are used as declared, case-sensitive |

## Type mapping

Create each field with the platform type the SDK will write:

| UPROPERTY | Platform field type |
|-----------|---------------------|
| `FString` | `text` |
| `int32`, `int64` | `integer` |
| `float`, `double` | `number` |
| `bool` | `boolean` |
| `UENUM` | `enum`, targeting the enum of the same name; declare every value the UENUM has |
| `USTRUCT` | `inclusion` of the part (cardinality one) |
| `TArray<USTRUCT>` | `inclusion` of the part (cardinality many) |
| `TArray<primitive>` | `json` |
| `TObjectPtr<Entity>` | `relation` to the target entity (cardinality one), saved as the record id |
| `TArray<TObjectPtr<Entity>>` | `relation` to the target entity (cardinality many, ordered) |
| entity reference inside a `USTRUCT` | `text` carrying the id |

A field the table does not declare is refused with `422 unknown_field`; an enum value the enum does not declare with `422 validation_failed` ([Errors and logging](Errors.md)). Add fields and enum values on the platform before the code that writes them ships.

## Access and ownership

Set per table where you create it:

- **Client read and client write** (the table's ACL): whether game clients, with the client key and a player session, may read and write rows. A table without client write is written only by servers (a dedicated server, a cloud function). `UCLASS(PlayServClientWritable)` ([Entities](Entities.md)) records the same intent in your code.
- **Ownership**: `owned_by: player` marks every row with the player who created it and limits access to that player; `read: owner`, the default for owned tables, hides other players' rows, and `read: public` lets anyone read while only the owner writes. An owned table needs an `on_player_delete` policy (`cascade-delete`, `restrict` or `anonymise`).
- **Player-owned classes**: for a `PlayServPlayerOwned` class ([Entities](Entities.md)) create the table owned by the player, and add a text field named `player_id`, `unique` and `required`, that your class does not declare: the SDK writes each row's player there and `LoadPlayerOwned` finds rows by it. Use `cascade-delete` as the player-delete policy: `anonymise` replaces only the owner and would leave the deleted player's id in `player_id`.
- **Singleton**: create the entity as a singleton (`singleton: true`) for a `PlayServSingleton` class ([Entities](Entities.md)), open to client reads if clients read it and closed to client writes. Give every required field a default, or the platform cannot create the row on its first read.

Unowned tables are shared by all players. *Finding your records* in [Data](Data.md) shows which shape suits which data.

## Changing a schema that has data

Adding a field, a value or a table is a plain edit. Changing a field's type, removing a field, or tightening a constraint on a table that already holds rows is refused with `409 requires_migration`; run it as a migration (propose → plan → apply, in the console or through the MCP), then update the C++ side. Renaming an entity class means a new table, and the old one keeps its rows ([Entities](Entities.md)).

## What the SDK checks

At the first login of a process the SDK fetches the table list once and logs a warning for every entity class that has no table on the platform, and for every class whose `PlayServSingleton` marking its table does not match; a clean run logs one line. The check changes nothing, but a forgotten table shows at startup rather than as a `NotFound` on the first `Save`. A data operation on a class with no table fails with an error naming the class.

## Later: pushing the schema from the editor

The manifest is the input for schema tooling that builds and updates the tables from your code, including the client-write intent your classes already declare. That tooling is not part of this release; classes written against the rules above need no change when it arrives.

---

Part of the PlayServ SDK documentation: [README](../README.md) lists every page.
