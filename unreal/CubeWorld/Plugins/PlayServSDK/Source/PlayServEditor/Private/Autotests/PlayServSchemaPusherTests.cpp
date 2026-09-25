#include "PlayServSchemaPusher.h"
#include "Dom/JsonObject.h"
#include "Misc/AutomationTest.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Friend-based access to FPlayServSchemaPusher::BuildPushDto — the FPlayServAuthTestAccess
 * pattern, in the same module. Keeps the seam private on the pusher while the tests below pin
 * the push body offline.
 */
class FPlayServSchemaPusherTestAccess
{
public:
	static TSharedPtr<FJsonObject> BuildPushDto(const TSharedPtr<FJsonObject>& Manifest, const TSharedPtr<FJsonObject>& Overlay, FString& OutError)
	{
		return FPlayServSchemaPusher::BuildPushDto(Manifest, Overlay, OutError);
	}
};

// PlayServ.Editor.SchemaPush.* — offline coverage of the push body the schema pusher builds
// from a manifest + overlay pair (no platform round trip; BuildPushDto is pure JSON in, JSON
// out). Until 2026-09-06 the editor module had no tests at all (drift register §3.4:
// "editor/schema push 0 tests / 34 rows").

namespace
{
	TSharedPtr<FJsonObject> ParseJson(const FString& Text)
	{
		TSharedPtr<FJsonObject> Json;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		return FJsonSerializer::Deserialize(Reader, Json) ? Json : nullptr;
	}

	const TCHAR* const ManifestWithEnum = TEXT(R"json({
		"manifestVersion": 2,
		"entities": [
			{
				"classPath": "/Script/PusherTest.PusherTestItem",
				"class": "UPusherTestItem",
				"module": "PusherTest",
				"mode": "whole",
				"clientWritable": true,
				"schemaHash": "0x00000001",
				"fields": [
					{ "name": "Name", "kind": 1, "kindName": "String", "clientWritable": false, "decl": "FString" },
					{ "name": "Rarity", "kind": 7, "kindName": "Enum", "clientWritable": false, "decl": "EPusherTestRank" }
				]
			}
		],
		"enums": [
			{ "name": "EPusherTestRank", "codeKey": "/Script/PusherTest.EPusherTestRank", "values": ["Bronze", "Silver", "Gold"] }
		]
	})json");

	const TCHAR* const ManifestWithoutEnums = TEXT(R"json({
		"manifestVersion": 2,
		"entities": [
			{
				"classPath": "/Script/PusherTest.PusherTestItem",
				"class": "UPusherTestItem",
				"module": "PusherTest",
				"mode": "whole",
				"clientWritable": true,
				"schemaHash": "0x00000001",
				"fields": [
					{ "name": "Name", "kind": 1, "kindName": "String", "clientWritable": false, "decl": "FString" }
				]
			}
		]
	})json");

	// The manifest exporter walks EVERY entity in the target, excluded ones included, so an enum
	// used only by an excluded fixture still appears here.
	const TCHAR* const ManifestWithUnreferencedEnum = TEXT(R"json({
		"manifestVersion": 2,
		"entities": [
			{
				"classPath": "/Script/PusherTest.PusherTestItem",
				"class": "UPusherTestItem",
				"module": "PusherTest",
				"mode": "whole",
				"clientWritable": true,
				"schemaHash": "0x00000001",
				"fields": [
					{ "name": "Name", "kind": 1, "kindName": "String", "clientWritable": false, "decl": "FString" }
				]
			},
			{
				"classPath": "/Script/PusherTest.PusherTestFixture",
				"class": "UPusherTestFixture",
				"module": "PusherTest",
				"mode": "whole",
				"clientWritable": true,
				"schemaHash": "0x00000002",
				"fields": [
					{ "name": "Rank", "kind": 7, "kindName": "Enum", "clientWritable": false, "decl": "EPusherTestRank" }
				]
			}
		],
		"enums": [
			{ "name": "EPusherTestRank", "codeKey": "/Script/PusherTest.EPusherTestRank", "values": ["Bronze", "Silver", "Gold"] }
		]
	})json");

	const TCHAR* const OverlayForItem = TEXT(R"json({
		"entities": { "/Script/PusherTest.PusherTestItem": { "clientRead": true } }
	})json");

	const TCHAR* const OverlayExcludingFixture = TEXT(R"json({
		"excludedClassPaths": [ "/Script/PusherTest.PusherTestFixture" ],
		"entities": { "/Script/PusherTest.PusherTestItem": { "clientRead": true } }
	})json");
	const TCHAR* const ManifestWithMarkings = TEXT(R"json({
		"manifestVersion": 2,
		"entities": [
			{
				"classPath": "/Script/PusherTest.PusherTestConfig",
				"class": "UPusherTestConfig",
				"module": "PusherTest",
				"mode": "whole",
				"clientWritable": false,
				"singleton": true,
				"owned": false,
				"schemaHash": "0x00000003",
				"fields": [
					{ "name": "Motd", "kind": 1, "kindName": "String", "clientWritable": false, "decl": "FString" }
				]
			},
			{
				"classPath": "/Script/PusherTest.PusherTestStanding",
				"class": "UPusherTestStanding",
				"module": "PusherTest",
				"mode": "whole",
				"clientWritable": true,
				"singleton": false,
				"owned": true,
				"schemaHash": "0x00000004",
				"fields": [
					{ "name": "Renown", "kind": 2, "kindName": "Int32", "clientWritable": false, "decl": "int32" }
				]
			},
			{
				"classPath": "/Script/PusherTest.PusherTestPublicStanding",
				"class": "UPusherTestPublicStanding",
				"module": "PusherTest",
				"mode": "whole",
				"clientWritable": false,
				"singleton": false,
				"owned": true,
				"schemaHash": "0x00000005",
				"fields": [
					{ "name": "Heat", "kind": 2, "kindName": "Int32", "clientWritable": false, "decl": "int32" }
				]
			}
		]
	})json");

	const TCHAR* const OverlayForMarkings = TEXT(R"json({
		"entities": {
			"/Script/PusherTest.PusherTestConfig": { "clientRead": true },
			"/Script/PusherTest.PusherTestStanding": { "clientRead": true },
			"/Script/PusherTest.PusherTestPublicStanding": { "clientRead": true, "read": "public" }
		}
	})json");

	const TCHAR* const OverlayAnonymisingStanding = TEXT(R"json({
		"entities": {
			"/Script/PusherTest.PusherTestConfig": { "clientRead": true },
			"/Script/PusherTest.PusherTestStanding": { "clientRead": true, "onPlayerDelete": "anonymise" },
			"/Script/PusherTest.PusherTestPublicStanding": { "clientRead": true, "read": "public" }
		}
	})json");

	TSharedPtr<FJsonObject> FindPushedEntity(const TSharedPtr<FJsonObject>& Dto, const FString& Name)
	{
		for (const TSharedPtr<FJsonValue>& Value : Dto->GetArrayField(TEXT("entities")))
		{
			const TSharedPtr<FJsonObject> Entity = Value->AsObject()->GetObjectField(TEXT("entity"));
			if (Entity->GetStringField(TEXT("name")) == Name)
			{
				return Entity;
			}
		}
		return nullptr;
	}

	TSharedPtr<FJsonObject> FindPushedField(const TSharedPtr<FJsonObject>& Entity, const FString& Name)
	{
		for (const TSharedPtr<FJsonValue>& Value : Entity->GetArrayField(TEXT("fields")))
		{
			if (Value->AsObject()->GetStringField(TEXT("name")) == Name)
			{
				return Value->AsObject();
			}
		}
		return nullptr;
	}
}

// The push body carries every enum the manifest declares as {code_key, enum:{name, values}},
// and the enum-typed field targets the very name the bundle declares — so a code-first project
// pushes clean against a fresh environment with no hand-created enum.
// Written RED: before the pusher emitted `enums`, this body had only entities and parts.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServSchemaPushEnumsFromManifestTest,
	"PlayServ.Editor.SchemaPush.EnumsFromManifest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServSchemaPushEnumsFromManifestTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Manifest = ParseJson(ManifestWithEnum);
	const TSharedPtr<FJsonObject> Overlay = ParseJson(OverlayForItem);
	TestTrue(TEXT("fixtures parse"), Manifest.IsValid() && Overlay.IsValid());
	if (!Manifest.IsValid() || !Overlay.IsValid())
	{
		return false;
	}

	FString Error;
	const TSharedPtr<FJsonObject> Dto = FPlayServSchemaPusherTestAccess::BuildPushDto(Manifest, Overlay, Error);
	TestTrue(FString::Printf(TEXT("DTO builds (error: '%s')"), *Error), Dto.IsValid());
	if (!Dto.IsValid())
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Enums = nullptr;
	TestTrue(TEXT("DTO carries an enums array"), Dto->TryGetArrayField(TEXT("enums"), Enums) && Enums != nullptr);
	if (Enums == nullptr)
	{
		return false;
	}
	TestEqual(TEXT("one enum pushed"), Enums->Num(), 1);
	if (Enums->Num() != 1)
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Wrapper = (*Enums)[0]->AsObject();
	TestEqual(TEXT("code_key is the enum's engine path"), Wrapper->GetStringField(TEXT("code_key")), FString(TEXT("/Script/PusherTest.EPusherTestRank")));
	const TSharedPtr<FJsonObject>* EnumDto = nullptr;
	TestTrue(TEXT("wrapper carries the enum"), Wrapper->TryGetObjectField(TEXT("enum"), EnumDto) && EnumDto != nullptr);
	if (EnumDto == nullptr)
	{
		return false;
	}
	TestEqual(TEXT("enum name keeps the E prefix"), (*EnumDto)->GetStringField(TEXT("name")), FString(TEXT("EPusherTestRank")));
	TArray<FString> Values;
	for (const TSharedPtr<FJsonValue>& Value : (*EnumDto)->GetArrayField(TEXT("values")))
	{
		Values.Add(Value->AsString());
	}
	TestEqual(TEXT("values in declaration order, short names"), FString::Join(Values, TEXT(",")), FString(TEXT("Bronze,Silver,Gold")));

	const TArray<TSharedPtr<FJsonValue>>& Entities = Dto->GetArrayField(TEXT("entities"));
	TestEqual(TEXT("one entity pushed"), Entities.Num(), 1);
	if (Entities.Num() != 1)
	{
		return false;
	}
	FString Target;
	for (const TSharedPtr<FJsonValue>& FieldValue : Entities[0]->AsObject()->GetObjectField(TEXT("entity"))->GetArrayField(TEXT("fields")))
	{
		const TSharedPtr<FJsonObject> Field = FieldValue->AsObject();
		if (Field->GetStringField(TEXT("name")) == TEXT("Rarity"))
		{
			Target = Field->GetStringField(TEXT("target"));
		}
	}
	TestEqual(TEXT("the enum field targets the declared enum by name"), Target, FString(TEXT("EPusherTestRank")));
	return true;
}

// A manifest with no `enums` member (an older exporter) still builds, with an
// EMPTY enums array — the platform's `enums` is optional-but-present in the body, never absent.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServSchemaPushNoEnumsIsEmptyArrayTest,
	"PlayServ.Editor.SchemaPush.NoEnumsIsEmptyArray",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServSchemaPushNoEnumsIsEmptyArrayTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Manifest = ParseJson(ManifestWithoutEnums);
	const TSharedPtr<FJsonObject> Overlay = ParseJson(OverlayForItem);
	TestTrue(TEXT("fixtures parse"), Manifest.IsValid() && Overlay.IsValid());
	if (!Manifest.IsValid() || !Overlay.IsValid())
	{
		return false;
	}

	FString Error;
	const TSharedPtr<FJsonObject> Dto = FPlayServSchemaPusherTestAccess::BuildPushDto(Manifest, Overlay, Error);
	TestTrue(FString::Printf(TEXT("DTO builds (error: '%s')"), *Error), Dto.IsValid());
	if (!Dto.IsValid())
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Enums = nullptr;
	TestTrue(TEXT("DTO carries an enums array even when the manifest has none"), Dto->TryGetArrayField(TEXT("enums"), Enums) && Enums != nullptr);
	TestEqual(TEXT("and it is empty"), Enums != nullptr ? Enums->Num() : -1, 0);
	return true;
}

// Only enums a PUSHED field targets ride the push. The exporter records every enum in the target,
// so an enum used solely by an excluded class (the fixture exclusions are the live case) must
// not be pushed: it would create an orphan on the platform and 409 against a hand-created enum
// of the same name that no pushed field even uses. Written RED.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServSchemaPushUnreferencedEnumNotPushedTest,
	"PlayServ.Editor.SchemaPush.UnreferencedEnumNotPushed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServSchemaPushUnreferencedEnumNotPushedTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Manifest = ParseJson(ManifestWithUnreferencedEnum);
	const TSharedPtr<FJsonObject> Overlay = ParseJson(OverlayExcludingFixture);
	TestTrue(TEXT("fixtures parse"), Manifest.IsValid() && Overlay.IsValid());
	if (!Manifest.IsValid() || !Overlay.IsValid())
	{
		return false;
	}

	FString Error;
	const TSharedPtr<FJsonObject> Dto = FPlayServSchemaPusherTestAccess::BuildPushDto(Manifest, Overlay, Error);
	TestTrue(FString::Printf(TEXT("DTO builds (error: '%s')"), *Error), Dto.IsValid());
	if (!Dto.IsValid())
	{
		return false;
	}

	TestEqual(TEXT("only the non-excluded entity is pushed"), Dto->GetArrayField(TEXT("entities")).Num(), 1);
	const TArray<TSharedPtr<FJsonValue>>* Enums = nullptr;
	TestTrue(TEXT("DTO carries an enums array"), Dto->TryGetArrayField(TEXT("enums"), Enums) && Enums != nullptr);
	TestEqual(TEXT("the enum only the excluded fixture uses is NOT pushed"), Enums != nullptr ? Enums->Num() : -1, 0);
	return true;
}

// UCLASS(PlayServSingleton) reaches the push as the entity's singleton flag, and
// UCLASS(PlayServEntity, PlayServPlayerOwned) as a player-owned table readable by its owner that is deleted with the
// player; the overlay still overrides a policy it names.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServSchemaPushClassMarkingsTest,
	"PlayServ.Editor.SchemaPush.SingletonAndOwnedFromManifest",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServSchemaPushClassMarkingsTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Manifest = ParseJson(ManifestWithMarkings);
	const TSharedPtr<FJsonObject> Overlay = ParseJson(OverlayForMarkings);
	FString Error;
	const TSharedPtr<FJsonObject> Dto = (Manifest.IsValid() && Overlay.IsValid()) ? FPlayServSchemaPusherTestAccess::BuildPushDto(Manifest, Overlay, Error) : nullptr;
	if (!TestTrue(FString::Printf(TEXT("DTO builds (error: '%s')"), *Error), Dto.IsValid()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Config = FindPushedEntity(Dto, TEXT("PusherTestConfig"));
	const TSharedPtr<FJsonObject> Standing = FindPushedEntity(Dto, TEXT("PusherTestStanding"));
	const TSharedPtr<FJsonObject> PublicStanding = FindPushedEntity(Dto, TEXT("PusherTestPublicStanding"));
	if (!TestTrue(TEXT("all three entities are pushed"), Config.IsValid() && Standing.IsValid() && PublicStanding.IsValid()))
	{
		return false;
	}

	bool bSingleton = false;
	TestTrue(TEXT("the singleton class is pushed as a singleton"), Config->TryGetBoolField(TEXT("singleton"), bSingleton) && bSingleton);
	TestFalse(TEXT("an ordinary class carries no singleton flag"), Standing->HasField(TEXT("singleton")));
	TestFalse(TEXT("a singleton is not player-owned"), Config->HasField(TEXT("owned_by")));

	FString Value;
	TestTrue(TEXT("an owned class is owned by the player"), Standing->TryGetStringField(TEXT("owned_by"), Value) && Value == TEXT("player"));
	TestTrue(TEXT("an owned class is readable by its owner"), Standing->TryGetStringField(TEXT("read"), Value) && Value == TEXT("owner"));
	TestTrue(TEXT("an owned class is deleted with its player"), Standing->TryGetStringField(TEXT("on_player_delete"), Value) && Value == TEXT("cascade-delete"));
	TestTrue(TEXT("the overlay still overrides the read policy"), PublicStanding->TryGetStringField(TEXT("read"), Value) && Value == TEXT("public"));
	return true;
}

// A player-owned table keeps each row's player in a player_id field the class does not declare; LoadPlayerOwned
// addresses the row by it, so the push adds it as unique, required text. No other field gets either flag, and a
// table that is not player-owned gets no player_id.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServSchemaPushPlayerIdFieldTest,
	"PlayServ.Editor.SchemaPush.PlayerOwnedAddsPlayerIdField",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServSchemaPushPlayerIdFieldTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Manifest = ParseJson(ManifestWithMarkings);
	const TSharedPtr<FJsonObject> Overlay = ParseJson(OverlayForMarkings);
	FString Error;
	const TSharedPtr<FJsonObject> Dto = (Manifest.IsValid() && Overlay.IsValid()) ? FPlayServSchemaPusherTestAccess::BuildPushDto(Manifest, Overlay, Error) : nullptr;
	const TSharedPtr<FJsonObject> Standing = Dto.IsValid() ? FindPushedEntity(Dto, TEXT("PusherTestStanding")) : nullptr;
	if (!TestTrue(FString::Printf(TEXT("the owned entity is pushed (error: '%s')"), *Error), Standing.IsValid()))
	{
		return false;
	}

	const TSharedPtr<FJsonObject> Key = FindPushedField(Standing, TEXT("player_id"));
	const TSharedPtr<FJsonObject> Other = FindPushedField(Standing, TEXT("Renown"));
	if (!TestTrue(TEXT("the class field and player_id are pushed"), Key.IsValid() && Other.IsValid()))
	{
		return false;
	}
	bool bFlag = false;
	FString Type;
	TestTrue(TEXT("player_id is text"), Key->TryGetStringField(TEXT("type"), Type) && Type == TEXT("text"));
	TestTrue(TEXT("player_id is unique"), Key->TryGetBoolField(TEXT("unique"), bFlag) && bFlag);
	TestTrue(TEXT("player_id is required"), Key->TryGetBoolField(TEXT("required"), bFlag) && bFlag);
	TestFalse(TEXT("another field is not unique"), Other->HasField(TEXT("unique")));
	TestFalse(TEXT("another field is not required"), Other->HasField(TEXT("required")));
	const TSharedPtr<FJsonObject> Config = FindPushedEntity(Dto, TEXT("PusherTestConfig"));
	TestFalse(TEXT("a table that is not player-owned gets no player_id"), Config.IsValid() && FindPushedField(Config, TEXT("player_id")).IsValid());
	return true;
}

// A player's deletion under `anonymise` rewrites only the row's owner, so the player_id field of a player-owned table
// would keep the deleted player's id. The push refuses that policy on a player-owned class instead of sending it.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServSchemaPushPlayerOwnedRefusesAnonymiseTest,
	"PlayServ.Editor.SchemaPush.PlayerOwnedRefusesAnonymise",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServSchemaPushPlayerOwnedRefusesAnonymiseTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<FJsonObject> Manifest = ParseJson(ManifestWithMarkings);
	const TSharedPtr<FJsonObject> Overlay = ParseJson(OverlayAnonymisingStanding);
	FString Error;
	const TSharedPtr<FJsonObject> Dto = (Manifest.IsValid() && Overlay.IsValid()) ? FPlayServSchemaPusherTestAccess::BuildPushDto(Manifest, Overlay, Error) : nullptr;
	TestFalse(TEXT("the push is refused"), Dto.IsValid());
	TestTrue(FString::Printf(TEXT("the refusal names the class (error: '%s')"), *Error), Error.Contains(TEXT("PusherTestStanding")));
	TestTrue(TEXT("and the policy"), Error.Contains(TEXT("anonymise")));
	return true;
}

#endif
