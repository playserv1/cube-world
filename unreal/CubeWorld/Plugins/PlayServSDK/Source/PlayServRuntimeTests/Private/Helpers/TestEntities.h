#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "Auth/PlayServAuth.h"
#include "TestEntities.generated.h"

// Enums

UENUM()
enum class ETestRank : uint8
{
	Bronze,
	Silver,
	Gold
};

// Value objects — inlined as nested JSON, not standalone entities

USTRUCT()
struct FTestProfile
{
	GENERATED_BODY()

	UPROPERTY()
	FString Bio;

	UPROPERTY()
	int32 Age = 0;

	UPROPERTY()
	bool bVerified = false;

	/** Transient field inside USTRUCT — must NOT be serialized to backend. */
	UPROPERTY(Transient)
	FString CachedDisplayName;
};

class UTestItem;

USTRUCT()
struct FInventorySlot
{
	GENERATED_BODY()

	UPROPERTY()
	TObjectPtr<UTestItem> Item;

	UPROPERTY()
	int32 Count = 0;

	UPROPERTY()
	FString Notes;
};

// Non-persistent UObject — used to verify serializer skips non-persistent refs
UCLASS()
class UTestTransient : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY()
	FString Label;
};

// Persistent entities
//
// Every CRUD-exercised fixture is UCLASS(PlayServEntity, PlayServClientWritable): the suite
// runs on the client plane as anonymous players, and these tables are UNOWNED shared rows —
// client-writable + no owner predicate. The marking is the ACL intent the schema push carries
// (acl.client.write=true).

class UTestClan;
class UTestInventory;

// UTestPlayer is the most widely used entity across the CRUD / Collection / Error / Queue /
// Filter suites, so the full suite passing is the regression signal that specifier detection
// works end-to-end with no member loss — refs serialize as string IDs, change tracking fires,
// and the entity is Save/Load/Reload/bulk-eligible.
UCLASS(PlayServEntity, PlayServClientWritable)
class UTestPlayer : public UObject
{
	GENERATED_BODY()

public:
	// Scalar types
	UPROPERTY()
	FString Name;

	UPROPERTY()
	int32 Level = 1;

	UPROPERTY()
	float Score = 0.0f;

	UPROPERTY()
	bool bActive = false;

	UPROPERTY()
	double Rating = 0.0;

	UPROPERTY()
	int64 PlayTime = 0;

	// Enum
	UPROPERTY()
	ETestRank Rank = ETestRank::Bronze;

	// Value object (USTRUCT)
	UPROPERTY()
	FTestProfile Profile;

	// Entity refs
	UPROPERTY()
	TObjectPtr<UTestClan> Clan;

	UPROPERTY()
	TObjectPtr<UTestInventory> Inventory;

	// Non-persistent ref — should be skipped by serializer
	UPROPERTY()
	TObjectPtr<UObject> Transient;

	/** Transient field — must NOT be serialized to backend. */
	UPROPERTY(Transient)
	FString LocalCacheData;
};

UCLASS(PlayServEntity, PlayServClientWritable)
class UTestClan : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY()
	FString ClanName;

	UPROPERTY()
	FString Tag;

	UPROPERTY()
	int32 MemberCount = 0;

	UPROPERTY()
	TArray<FString> Tags;

	UPROPERTY()
	TArray<int32> MemberScores;

	// Array of entity refs
	UPROPERTY()
	TArray<TObjectPtr<UTestPlayer>> Players;
};

UCLASS(PlayServEntity, PlayServClientWritable)
class UTestItem : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY()
	FString ItemName;

	UPROPERTY()
	int32 Power = 0;

	UPROPERTY()
	ETestRank Rarity = ETestRank::Bronze;
};

UCLASS(PlayServEntity, PlayServClientWritable)
class UTestInventory : public UObject
{
	GENERATED_BODY()

public:
	// TArray<USTRUCT> with nested entity refs
	UPROPERTY()
	TArray<FInventorySlot> Slots;

	UPROPERTY()
	int32 MaxSlots = 10;
};

// Entity for the reload-overwrite tests (PlayServ.Data.Reload.*). The name is historical; the
// class name is KEPT because it is registered in the platform schema and a rename would be a
// schema drop+add. Field shapes cover scalars, a primitive TArray, a nested USTRUCT, FP
// precision (no-op reload drift guard), and a Transient field the serializer must skip.
UCLASS(PlayServEntity, PlayServClientWritable)
class UTestNotifyEntity : public UObject
{
	GENERATED_BODY()

public:
	// Scalars (Title + Count cover single-field and multi-field diffs)
	UPROPERTY(PlayServOnChanged = OnTitleChanged)
	FString Title;

	UPROPERTY(PlayServOnChanged = OnCountChanged)
	int32 Count = 0;

	// Primitive array — a changed element must report the single owning top-level field
	UPROPERTY()
	TArray<int32> Scores;

	// Floating-point scalars — a no-op reload must NOT report these (guards JSON round-trip drift,
	// the likeliest spurious-diff source). float + double cover both precisions.
	UPROPERTY()
	float Ratio = 0.0f;

	UPROPERTY()
	double Precise = 0.0;

	// Nested USTRUCT — a changed member must report the top-level field, not a nested path
	UPROPERTY(PlayServOnChanged = OnProfileChanged)
	FTestProfile Profile;

	// Transient — differs locally but must never appear in the change list
	UPROPERTY(Transient)
	FString LocalOnly;

	// Every change callback, in call order, and whatever a test's Subscribe delegate appends.
	TArray<FString> Calls;

	UFUNCTION()
	void OnTitleChanged(const FString& OldTitle)
	{
		Calls.Add(FString::Printf(TEXT("Title:%s"), *OldTitle));
	}

	UFUNCTION()
	void OnCountChanged()
	{
		Calls.Add(TEXT("Count"));
	}

	UFUNCTION()
	void OnProfileChanged(const FTestProfile& OldProfile)
	{
		Calls.Add(FString::Printf(TEXT("Profile:%s"), *OldProfile.Bio));
	}
};

// --- Detection matrix ---
// Purpose-built, field-free entities covering the persistence-detection cases for
// PlayServ.Data.Persistence.Detection. Kept separate from the CRUD/Reload entities so
// the unit test reads as a clean truth table and does not depend on incidental entities.

// Specifier-marked. The name is historical; KEPT because the class is registered in the
// platform schema and a rename would be a schema drop+add.
UCLASS(PlayServEntity, PlayServClientWritable)
class UTestDetectBaseDerived : public UObject
{
	GENERATED_BODY()
};

// Plain unmarked UObject → NOT detected (no specifier).
UCLASS()
class UTestDetectPlainNoMacro : public UObject
{
	GENERATED_BODY()
};


// --- Codegen-marking matrix (0.4.0) ---
// Compile-time marking fixtures for PlayServ.Data.Codegen.*. The bare specifier parses
// because the PlayServUht ubtplugin is always loaded wherever the plugin is enabled
// (native placement under Plugins/). PLAYSERV_PERSISTENT and its runtime registry were
// removed 2026-08-13; the reflection fallback survives for Blueprint-ancestry entities.

// Plain UObject with the bare compile-time specifier.
// No ancestry: persistence detection and serialization run entirely on codegen.
UCLASS(PlayServEntity, PlayServClientWritable)
class UTestSpecifierEntity : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY()
	FString Label;

	UPROPERTY()
	int32 Value = 0;

	// Array under the descriptor path (UTestClan's arrays only cover the reflection path).
	UPROPERTY()
	TArray<int32> Numbers;

	/** Transient — the generated descriptor must exclude it, same as the reflection walk. */
	UPROPERTY(Transient)
	FString ScratchNotSaved;
};

// meta=() spelling — must behave identically to the bare specifier. Stays deliberately
// UNwritable: the pair (SpecifierEntity writable, this one not) keeps both ACL polarities
// covered in the pushed schema.
UCLASS(meta=(PlayServEntity))
class UTestSpecifierMetaEntity : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY()
	int32 Amount = 0;
};

// SERVER-ONLY table: whole-class marking, never client-writable, and the push overlay also
// closes client READ (clientRead: false). The one client-closed fixture the ACL-denial tests
// read — every other fixture is client-readable by the pusher's default policy.
UCLASS(PlayServEntity)
class UTestServerOnlyEntity : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY()
	FString Note;
};

// --- Marking-model fixtures ---

// PARTIAL entity: no PlayServEntity on the class; >=1 UPROPERTY(PlayServProperty). Only the
// marked properties serialize/register; the unmarked ones are invisible to persistence.
UCLASS()
class UTestPartialEntity : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(PlayServProperty)
	FString PersistedName;

	UPROPERTY(PlayServProperty, PlayServClientWritable)
	int32 PersistedScore = 0;

	// Unmarked — must NOT appear in the descriptor, the manifest, or any serialized document.
	UPROPERTY()
	FString LocalNote;

	UPROPERTY()
	int32 LocalCounter = 0;
};

// WHOLE entity with client-writability: entity-level intent plus one per-field marker on top
// of whole-class marking (allowed — the field marker narrows nothing, it adds write intent).
UCLASS(PlayServEntity, PlayServClientWritable)
class UTestWritableEntity : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY(PlayServClientWritable)
	int32 OwnedScore = 0;

	UPROPERTY()
	FString ServerManaged;
};

// --- Scope fixtures: one row for the whole project, one row per player ---

// The singleton the suite reads and writes on the server plane. Client-readable, never client-writable.
UCLASS(PlayServSingleton)
class UTestConfigSingleton : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY()
	FString Motd;

	UPROPERTY()
	int32 MaxPlayers = 8;

	UPROPERTY()
	float XpMultiplier = 1.0f;

	UPROPERTY()
	bool bEventActive = false;
};

// A player's own row: player-owned, owner-read, open to client writes.
UCLASS(PlayServEntity, PlayServClientWritable, PlayServPlayerOwned)
class UTestPlayerProfile : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY()
	FString Nickname;

	UPROPERTY()
	int32 Coins = 0;
};

// A player's row only servers write: player-owned, owner-read, closed to client writes.
UCLASS(PlayServEntity, PlayServPlayerOwned)
class UTestPlayerStanding : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY()
	int32 Renown = 0;
};

/** Test helper: receives OnSessionLost broadcast and records it. */
UCLASS()
class UPlayServSessionLostListener : public UObject
{
	GENERATED_BODY()

public:
	bool bSessionLostFired = false;

	UFUNCTION()
	void OnSessionLost() { bSessionLostFired = true; }
};
