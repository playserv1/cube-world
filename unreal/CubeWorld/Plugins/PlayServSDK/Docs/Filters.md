# Filters

`FPlayServFilter` selects records for `LoadAll` and `DeleteAll`, and for the raw `GetAll` and `DeleteAll` ([Data](Data.md)).

```cpp
#include "Data/PlayServFilter.h"   // included by PlayServ.h

// Field names are the UPROPERTY names you declared in C++, as written.
// For UPROPERTY() int32 Level:  Where(TEXT("Level"))

// Single condition:
FPlayServFilter::Where(TEXT("Level")).GreaterThan(10)

// AND chain:
FPlayServFilter::Where(TEXT("Owner")).EqualTo(PlayerId)
    .And(TEXT("Rarity")).In({TEXT("epic"), TEXT("legendary")})

// Nested field inside a USTRUCT, via dot notation:
FPlayServFilter::Where(TEXT("Profile.Age")).GreaterThanOrEqual(18)

// Null checks on a reference field:
FPlayServFilter::Where(TEXT("Clan")).IsNull()

// Unfiltered (returns all):
FPlayServFilter::None()

// Every record, said on purpose: the only way DeleteAll deletes everything
FPlayServFilter::All()
```

Every condition must hold; there is no OR.

## Operators

They follow `Where()` or `And()`; `.EqualTo(...)` without one does not compile.

| Operator | Overloads |
|----------|-----------|
| `EqualTo` | `FString`, `int32`, `double`, `bool` |
| `NotEqualTo` | `FString`, `int32` |
| `GreaterThan` | `int32`, `double` |
| `GreaterThanOrEqual` | `int32`, `double` |
| `LessThan` | `int32`, `double` |
| `LessThanOrEqual` | `int32`, `double` |
| `In` | `TArray<FString>` (an empty array matches nothing and triggers an `ensure`) |
| `IsNull` / `IsNotNull` | — |

## Nested paths

A dotted path reaches a field inside a `USTRUCT`: every segment but the last names a struct field, and the last names a field of that struct. Segments are case-sensitive, a path has at most 8 segments, and a path to a field that does not exist fails with `ValidationFailed`. A segment through an array of structs matches when any element does: `Where(TEXT("Loadout.Hp")).NotEqualTo(10)` selects records with at least one element whose `Hp` is not 10.

## Validation

The platform validates every filter against the table's schema. A condition on a field that does not exist fails the call with `EPlayServErrorCode::ValidationFailed` and the offending field named in `Message`.

## Safety guard on DeleteAll

`DeleteAll` refuses a default `FPlayServFilter()` and `FPlayServFilter::None()`. To delete every record, pass `FPlayServFilter::All()`, so a forgotten filter never empties a table. `LoadAll` with `None()` returns the whole table.

---

Part of the PlayServ SDK documentation: [README](../README.md) lists every page.
