#include "PlayServTestCommon.h"
#include "Code/PlayServRpcConverter.h"
#include "Data/PlayServData.h"
#include "PlayServRpcTestTypes.h"
#include "TestEntities.h"
#include "PlayServTestAccess.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/ScopeExit.h"

#if !UE_BUILD_SHIPPING

// Compile-time proof that the PlayServ code generator processed both marking spellings — the
// bare specifier and meta=(PlayServEntity) — including UTestPlayer, the most widely used
// fixture.
static_assert(UTestSpecifierEntity::bPlayServCodegen, "PlayServ codegen marker missing on bare-specifier entity");
static_assert(UTestSpecifierMetaEntity::bPlayServCodegen, "PlayServ codegen marker missing on meta-marked entity");
static_assert(UTestPlayer::bPlayServCodegen, "PlayServ codegen marker missing on reparented entity");

// ---------------------------------------------------------------------------
// PlayServ.Data.Codegen.Registration
//
// A plain UObject marked with UCLASS(PlayServEntity) counts as persistent entirely via the
// generated registration — the ONLY route. Truth table:
//
//   bare specifier, plain UObject   → descriptor + persistent
//   meta=() spelling, plain UObject → descriptor + persistent (spellings converge)
//   unmarked / non-persistent       → neither
//
// Synchronous — static class detection only, no login / backend.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServCodegenRegistrationTest,
	"PlayServ.Data.Codegen.Registration",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServCodegenRegistrationTest::RunTest(const FString& Parameters)
{
	// Compile-time descriptors exist for every codegen-visible marking route.
	TestTrue(TEXT("Bare-specifier entity has a codegen descriptor"),
		FPlayServDataTestAccess::HasCodegenDescriptor(UTestSpecifierEntity::StaticClass()));
	TestTrue(TEXT("meta=(PlayServEntity) entity has a codegen descriptor"),
		FPlayServDataTestAccess::HasCodegenDescriptor(UTestSpecifierMetaEntity::StaticClass()));
	TestTrue(TEXT("Reparented entity (UTestPlayer) has a codegen descriptor"),
		FPlayServDataTestAccess::HasCodegenDescriptor(UTestPlayer::StaticClass()));
	if (const UClass* HostGameEntity = FindObject<UClass>(nullptr, TEXT("/Script/UnrealTestProject.BlobPlayerProfile")))
	{
		TestTrue(TEXT("host-game entity (found by path, no include) has a codegen descriptor with zero source change"),
			FPlayServDataTestAccess::HasCodegenDescriptor(HostGameEntity));
	}
	else
	{
		AddWarning(TEXT("host game module not loaded — game-module codegen direction not asserted this run"));
	}

	TestTrue(TEXT("Specifier-flipped former macro entity (UTestClan) now has a codegen descriptor"),
		FPlayServDataTestAccess::HasCodegenDescriptor(UTestClan::StaticClass()));
	TestFalse(TEXT("Unmarked plain UObject has no codegen descriptor"),
		FPlayServDataTestAccess::HasCodegenDescriptor(UTestDetectPlainNoMacro::StaticClass()));
	TestFalse(TEXT("Non-persistent helper class has no codegen descriptor"),
		FPlayServDataTestAccess::HasCodegenDescriptor(UTestTransient::StaticClass()));

	// THE substitution proof: specifier-only classes are persistent with no macro and no
	// base-class ancestry — persistence detection runs entirely on the compile-time registration.
	TestTrue(TEXT("Bare-specifier entity is persistent via codegen alone"),
		UPlayServData::IsRegisteredPersistentClass(UTestSpecifierEntity::StaticClass()));
	TestTrue(TEXT("meta-marked entity is persistent via codegen alone"),
		UPlayServData::IsRegisteredPersistentClass(UTestSpecifierMetaEntity::StaticClass()));

	TestTrue(TEXT("Specifier-flipped UTestClan is persistent via codegen"),
		UPlayServData::IsRegisteredPersistentClass(UTestClan::StaticClass()));
	TestFalse(TEXT("Unmarked plain UObject remains non-persistent"),
		UPlayServData::IsRegisteredPersistentClass(UTestDetectPlainNoMacro::StaticClass()));

	// This target compiles at least the known entity classes into generated registrations
	// (test fixtures + the host project's entities). Logged for triage, asserted as a floor not an exact count.
	const int32 CodegenCount = FPlayServDataTestAccess::CodegenClassCount();
	AddInfo(FString::Printf(TEXT("Codegen registered %d entity classes"), CodegenCount));
	TestTrue(TEXT("Codegen registered at least the known entity classes"), CodegenCount >= 12);

	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Data.Codegen.SerializationEquivalence (0.4.0)
//
// The descriptor-driven serializer walk must produce byte-for-byte the same JSON as the
// legacy reflection walk — same fields, same order-independent values, same skips
// (CPF_Transient, editor-only, non-persistent refs, transient struct members). Exercised
// on the richest fixtures: UTestPlayer (scalars, enum, nested struct, entity refs, null
// ref, non-persistent ref, transient fields) and UTestSpecifierEntity (bare specifier,
// primitive array, transient). Deserialization is checked the same way: one JSON document
// hydrated through both walks must yield identical re-serialized state, including entity
// ref stubs carrying their string IDs.
//
// Synchronous — serializer only, no login / backend.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServCodegenSerializationEquivalenceTest,
	"PlayServ.Data.Codegen.SerializationEquivalence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServCodegenSerializationEquivalenceTest::RunTest(const FString& Parameters)
{
	// Whatever happens below, never leak the forced-reflection switch into other tests.
	ON_SCOPE_EXIT
	{
		FPlayServDataTestAccess::SetForceReflectionSerialization(false);
	};

	// --- Fixture: a UTestPlayer touching every serializer rule ---
	UTestPlayer* Player = NewObject<UTestPlayer>();
	Player->Name = TEXT("Equivalence");
	Player->Level = 42;
	Player->Score = 3.5f;
	Player->bActive = true;
	Player->Rating = 99.25;
	Player->PlayTime = 123456789;
	Player->Rank = ETestRank::Gold;
	Player->Profile.Bio = TEXT("bio text");
	Player->Profile.Age = 33;
	Player->Profile.bVerified = true;
	Player->Profile.CachedDisplayName = TEXT("must-not-serialize");   // Transient struct member
	Player->LocalCacheData = TEXT("must-not-serialize");              // Transient top-level field

	UTestClan* Clan = NewObject<UTestClan>();
	FPlayServDataTestAccess::BindRecordId(Clan, TEXT("clan-123"));
	Player->Clan = Clan;                                              // entity ref → string ID
	Player->Inventory = nullptr;                                      // null entity ref → JSON null
	Player->Transient = NewObject<UTestTransient>();                  // non-persistent ref → skipped

	TestTrue(TEXT("Precondition: UTestPlayer runs the descriptor path"),
		FPlayServDataTestAccess::HasCodegenDescriptor(UTestPlayer::StaticClass()));

	// --- Serialize through both walks ---
	TSharedPtr<FJsonObject> DescriptorJson = FPlayServDataTestAccess::SerializeToJson(Player);
	FPlayServDataTestAccess::SetForceReflectionSerialization(true);
	TSharedPtr<FJsonObject> ReflectionJson = FPlayServDataTestAccess::SerializeToJson(Player);
	FPlayServDataTestAccess::SetForceReflectionSerialization(false);

	if (!TestTrue(TEXT("Descriptor serialization produced JSON"), DescriptorJson.IsValid()) ||
		!TestTrue(TEXT("Reflection serialization produced JSON"), ReflectionJson.IsValid()))
	{
		return false;
	}

	TestTrue(TEXT("Descriptor and reflection walks produce identical JSON (UTestPlayer)"),
		FJsonValue::CompareEqual(FJsonValueObject(DescriptorJson), FJsonValueObject(ReflectionJson)));

	// Spot-check the contract the equivalence rides on.
	TestTrue(TEXT("Entity ref serialized as its string ID"),
		DescriptorJson->HasTypedField<EJson::String>(TEXT("Clan")) && DescriptorJson->GetStringField(TEXT("Clan")) == TEXT("clan-123"));
	TestTrue(TEXT("Null entity ref serialized as JSON null"),
		DescriptorJson->HasTypedField<EJson::Null>(TEXT("Inventory")));
	TestFalse(TEXT("Non-persistent ref is skipped"), DescriptorJson->HasField(TEXT("Transient")));
	TestFalse(TEXT("CPF_Transient top-level field is skipped"), DescriptorJson->HasField(TEXT("LocalCacheData")));
	const TSharedPtr<FJsonObject>* ProfileJson = nullptr;
	if (TestTrue(TEXT("Nested struct is inlined"), DescriptorJson->TryGetObjectField(TEXT("Profile"), ProfileJson)))
	{
		TestFalse(TEXT("Transient struct member is skipped"), (*ProfileJson)->HasField(TEXT("CachedDisplayName")));
	}

	// --- Bare-specifier entity: array + transient exclusion under the descriptor path ---
	UTestSpecifierEntity* Specifier = NewObject<UTestSpecifierEntity>();
	Specifier->Label = TEXT("specified");
	Specifier->Value = 7;
	Specifier->Numbers = { 1, 2, 3 };
	Specifier->ScratchNotSaved = TEXT("must-not-serialize");

	TSharedPtr<FJsonObject> SpecifierDescriptorJson = FPlayServDataTestAccess::SerializeToJson(Specifier);
	FPlayServDataTestAccess::SetForceReflectionSerialization(true);
	TSharedPtr<FJsonObject> SpecifierReflectionJson = FPlayServDataTestAccess::SerializeToJson(Specifier);
	FPlayServDataTestAccess::SetForceReflectionSerialization(false);

	TestTrue(TEXT("Descriptor and reflection walks produce identical JSON (UTestSpecifierEntity)"),
		FJsonValue::CompareEqual(FJsonValueObject(SpecifierDescriptorJson), FJsonValueObject(SpecifierReflectionJson)));
	TestTrue(TEXT("Array field survives the descriptor path"),
		SpecifierDescriptorJson->HasTypedField<EJson::Array>(TEXT("Numbers")) && SpecifierDescriptorJson->GetArrayField(TEXT("Numbers")).Num() == 3);
	TestFalse(TEXT("Transient field is excluded from the generated descriptor"),
		SpecifierDescriptorJson->HasField(TEXT("ScratchNotSaved")));

	// --- Deserialization equivalence: same document through both walks ---
	UTestPlayer* HydratedViaDescriptor = NewObject<UTestPlayer>();
	UTestPlayer* HydratedViaReflection = NewObject<UTestPlayer>();

	TestTrue(TEXT("Descriptor deserialization succeeds"),
		FPlayServDataTestAccess::DeserializeFromJson(DescriptorJson, HydratedViaDescriptor));
	FPlayServDataTestAccess::SetForceReflectionSerialization(true);
	TestTrue(TEXT("Reflection deserialization succeeds"),
		FPlayServDataTestAccess::DeserializeFromJson(DescriptorJson, HydratedViaReflection));

	// Compare hydrated state through ONE walk (reflection) so the comparison itself is
	// path-neutral; entity-ref stubs serialize back as their IDs, making refs comparable.
	TSharedPtr<FJsonObject> RoundTripA = FPlayServDataTestAccess::SerializeToJson(HydratedViaDescriptor);
	TSharedPtr<FJsonObject> RoundTripB = FPlayServDataTestAccess::SerializeToJson(HydratedViaReflection);
	FPlayServDataTestAccess::SetForceReflectionSerialization(false);

	TestTrue(TEXT("Hydrated state identical through both walks"),
		FJsonValue::CompareEqual(FJsonValueObject(RoundTripA), FJsonValueObject(RoundTripB)));

	// Entity-ref hydration produced a stub carrying the string ID.
	if (TestNotNull(TEXT("Entity ref hydrated to a stub"), HydratedViaDescriptor->Clan.Get()))
	{
		TestEqual(TEXT("Hydrated stub carries the entity ID"),
			UPlayServData::GetRecordId(HydratedViaDescriptor->Clan.Get()), FString(TEXT("clan-123")));
	}

	return true;
}

// Injector coverage for the partial route: codegen processed the class even though it has no
// class-level marker.
static_assert(UTestPartialEntity::bPlayServCodegen, "PlayServ codegen marker missing on partial entity");

// ---------------------------------------------------------------------------
// PlayServ.Data.Codegen.MarkingModel
//
// The marking-model facts flow into descriptors and behavior:
//   partial entity   → persistent; descriptor lists ONLY marked fields; serialization and
//                      hydration are limited to them by construction
//   writable flags   → entity-level (UCLASS(PlayServClientWritable)) and per-field
//                      (UPROPERTY(PlayServClientWritable)) flags land in the descriptor
//
// Synchronous — descriptors + serializer only, no login / backend.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServCodegenMarkingModelTest,
	"PlayServ.Data.Codegen.MarkingModel",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServCodegenMarkingModelTest::RunTest(const FString& Parameters)
{
	// --- Partial entity: detection + descriptor shape ---
	TestTrue(TEXT("Partial entity is persistent"),
		UPlayServData::IsRegisteredPersistentClass(UTestPartialEntity::StaticClass()));
	TestTrue(TEXT("Partial entity descriptor flags partial mode"),
		FPlayServDataTestAccess::ClassIsPartial(UTestPartialEntity::StaticClass()));
	TestFalse(TEXT("Partial entity is not entity-level client-writable"),
		FPlayServDataTestAccess::ClassClientWritable(UTestPartialEntity::StaticClass()));
	TestEqual(TEXT("Partial descriptor lists exactly the two marked fields"),
		FPlayServDataTestAccess::DescriptorFieldCount(UTestPartialEntity::StaticClass()), 2);
	TestTrue(TEXT("Marked+writable field carries the per-field flag"),
		FPlayServDataTestAccess::FieldClientWritable(UTestPartialEntity::StaticClass(), GET_MEMBER_NAME_CHECKED(UTestPartialEntity, PersistedScore)));
	TestFalse(TEXT("Marked non-writable field does not"),
		FPlayServDataTestAccess::FieldClientWritable(UTestPartialEntity::StaticClass(), GET_MEMBER_NAME_CHECKED(UTestPartialEntity, PersistedName)));

	// --- Partial entity: serialization limited to marked fields ---
	UTestPartialEntity* Partial = NewObject<UTestPartialEntity>();
	Partial->PersistedName = TEXT("kept");
	Partial->PersistedScore = 7;
	Partial->LocalNote = TEXT("must-not-serialize");
	Partial->LocalCounter = 99;

	TSharedPtr<FJsonObject> PartialJson = FPlayServDataTestAccess::SerializeToJson(Partial);
	if (TestTrue(TEXT("Partial entity serialized"), PartialJson.IsValid()))
	{
		TestTrue(TEXT("Marked field serialized"), PartialJson->HasField(TEXT("PersistedName")));
		TestTrue(TEXT("Marked writable field serialized"), PartialJson->HasField(TEXT("PersistedScore")));
		TestFalse(TEXT("Unmarked field NOT serialized"), PartialJson->HasField(TEXT("LocalNote")));
		TestFalse(TEXT("Unmarked field NOT serialized (2)"), PartialJson->HasField(TEXT("LocalCounter")));
		TestEqual(TEXT("Exactly the marked fields serialized"), PartialJson->Values.Num(), 2);
	}

	// --- Partial entity: hydration ignores unmarked keys in the document ---
	TSharedPtr<FJsonObject> Doc = MakeShared<FJsonObject>();
	Doc->SetStringField(TEXT("PersistedName"), TEXT("from-server"));
	Doc->SetNumberField(TEXT("PersistedScore"), 42);
	Doc->SetStringField(TEXT("LocalNote"), TEXT("must-not-hydrate"));

	UTestPartialEntity* Hydrated = NewObject<UTestPartialEntity>();
	Hydrated->LocalNote = TEXT("untouched");
	TestTrue(TEXT("Partial hydration succeeds"), FPlayServDataTestAccess::DeserializeFromJson(Doc, Hydrated));
	TestEqual(TEXT("Marked field hydrated"), Hydrated->PersistedName, FString(TEXT("from-server")));
	TestEqual(TEXT("Marked writable field hydrated"), Hydrated->PersistedScore, 42);
	TestEqual(TEXT("Unmarked field untouched by hydration"), Hydrated->LocalNote, FString(TEXT("untouched")));

	// --- Whole entity with writability flags ---
	TestFalse(TEXT("Whole writable entity is not partial"),
		FPlayServDataTestAccess::ClassIsPartial(UTestWritableEntity::StaticClass()));
	TestTrue(TEXT("UCLASS(PlayServClientWritable) lands as the entity flag"),
		FPlayServDataTestAccess::ClassClientWritable(UTestWritableEntity::StaticClass()));
	TestTrue(TEXT("Per-field writable flag lands"),
		FPlayServDataTestAccess::FieldClientWritable(UTestWritableEntity::StaticClass(), GET_MEMBER_NAME_CHECKED(UTestWritableEntity, OwnedScore)));
	TestFalse(TEXT("Unflagged field stays non-writable"),
		FPlayServDataTestAccess::FieldClientWritable(UTestWritableEntity::StaticClass(), GET_MEMBER_NAME_CHECKED(UTestWritableEntity, ServerManaged)));

	// --- Unwritable whole entity (the ACL polarity pair of the fixtures) ---
	TestFalse(TEXT("Unmarked-writability entity is not client-writable"),
		FPlayServDataTestAccess::ClassClientWritable(UTestSpecifierMetaEntity::StaticClass()));
	TestFalse(TEXT("Whole entity is not partial"),
		FPlayServDataTestAccess::ClassIsPartial(UTestPlayer::StaticClass()));

	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Data.Codegen.WireNamesAreAuthored
//
// Every serialized field is keyed on the wire by the codegen table's compile-time literal, not
// by FProperty::GetName().
//
// Why: FName comparison is case-insensitive, and WITH_CASE_PRESERVING_NAME is
// WITH_EDITORONLY_DATA. Outside the editor an FName therefore carries no casing of its own —
// every spelling shares one name-table entry and ToString() returns whichever spelling the
// PROCESS registered first. A game that creates FName("SERVERNAME") before serializing makes a
// ServerName property report itself as SERVERNAME, and the platform refuses the write as an
// undeclared field. That is exactly what stopped Atone's dedicated server registering its
// session (Data spec, 2026-09-07).
//
// HONEST LIMIT: this test cannot go RED in the editor, where case preservation is ON and the
// defect is unreproducible by construction. It pins the WIRING that makes a packaged build
// correct, and it registers the colliding spellings first so that it DOES go red if this ever
// runs in a non-editor configuration — which is the point of the compiler/platform axis added
// to the engine-support policy on the same date.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServCodegenWireNamesTest,
	"PlayServ.Data.Codegen.WireNamesAreAuthored",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServCodegenWireNamesTest::RunTest(const FString& Parameters)
{
	// Claim the name-table entries under the WRONG casing before anything serializes. In a
	// packaged build these decide what FProperty::GetName() returns for the fixture's properties.
	const FName Collisions[] = { FName(TEXT("NAME")), FName(TEXT("LEVEL")), FName(TEXT("BACTIVE")),
		FName(TEXT("PLAYTIME")), FName(TEXT("RATING")) };
	// Touch the array so the registrations survive any optimisation level.
	TestTrue(TEXT("colliding spellings are in the name table"), Collisions[0] != NAME_None);

	UTestPlayer* Player = NewObject<UTestPlayer>();
	Player->Name = TEXT("Casing");
	Player->Level = 7;
	Player->PlayTime = 42;

	const TSharedPtr<FJsonObject> Json = FPlayServDataTestAccess::SerializeToJson(Player);
	if (!Json.IsValid())
	{
		AddError(TEXT("serializer returned no object"));
		return false;
	}

	// The declared spelling is on the wire, and the colliding spelling is not.
	const TCHAR* const Declared[] = { TEXT("Name"), TEXT("Level"), TEXT("bActive"), TEXT("PlayTime"), TEXT("Rating") };
	const TCHAR* const Colliding[] = { TEXT("NAME"), TEXT("LEVEL"), TEXT("BACTIVE"), TEXT("PLAYTIME"), TEXT("RATING") };
	for (int32 i = 0; i < UE_ARRAY_COUNT(Declared); ++i)
	{
		bool bFoundDeclared = false;
		bool bFoundColliding = false;
		for (const auto& Pair : Json->Values)
		{
			const FString Key = FString(*Pair.Key);
			// Case-SENSITIVE comparison: the whole point is which spelling was written.
			if (Key.Equals(Declared[i], ESearchCase::CaseSensitive))
			{
				bFoundDeclared = true;
			}
			if (Key.Equals(Colliding[i], ESearchCase::CaseSensitive))
			{
				bFoundColliding = true;
			}
		}
		TestTrue(FString::Printf(TEXT("'%s' is on the wire as declared"), Declared[i]), bFoundDeclared);
		TestFalse(FString::Printf(TEXT("'%s' is NOT on the wire"), Colliding[i]), bFoundColliding);
	}

	// Round-trip: the read path must key by the same authored spelling.
	UTestPlayer* Restored = NewObject<UTestPlayer>();
	TestTrue(TEXT("deserialize succeeds"), FPlayServDataTestAccess::DeserializeFromJson(Json, Restored));
	TestEqual(TEXT("Name round-trips"), Restored->Name, Player->Name);
	TestEqual(TEXT("Level round-trips"), Restored->Level, Player->Level);
	TestEqual(TEXT("PlayTime round-trips"), Restored->PlayTime, Player->PlayTime);

	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Data.Codegen.StructMemberNamesAreDeclared
//
// The members of a nested USTRUCT (a schema part) and of a cloud-function request/response struct
// are keyed on the wire by the codegen table's declared names, not by FProperty::GetName() — the
// same packaged-build casing hazard WireNamesAreAuthored covers for top-level fields.
//
// The editor preserves FName casing, so the hazard itself cannot be reproduced here. The test swaps
// the table's names for recased ones and checks the recased names reach the wire: that is red for
// any serializer that ignores the table, and it proves the table is the source rather than
// GetName(). The real names are restored before the test returns.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServCodegenStructMemberNamesTest,
	"PlayServ.Data.Codegen.StructMemberNamesAreDeclared",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServCodegenStructMemberNamesTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("codegen recorded the members of a nested part (FTestProfile)"), FPlayServDataTestAccess::HasStructMemberNames(FTestProfile::StaticStruct()));
	TestTrue(TEXT("codegen recorded the members of a cloud-function struct (FRpcTestInner)"), FPlayServDataTestAccess::HasStructMemberNames(FRpcTestInner::StaticStruct()));

	static const TCHAR* const ProfileDeclared[] = { TEXT("Bio"), TEXT("Age"), TEXT("bVerified"), TEXT("CachedDisplayName") };
	static const TCHAR* const ProfileRecased[] = { TEXT("bio"), TEXT("age"), TEXT("bverified"), TEXT("cacheddisplayname") };
	static const TCHAR* const InnerDeclared[] = { TEXT("Label"), TEXT("Weight") };
	static const TCHAR* const InnerRecased[] = { TEXT("label"), TEXT("weight") };
	const FString ProfilePath = FTestProfile::StaticStruct()->GetPathName();
	const FString InnerPath = FRpcTestInner::StaticStruct()->GetPathName();

	auto HasKey = [](const TSharedPtr<FJsonObject>& Object, const TCHAR* Key)
	{
		for (const auto& Pair : Object->Values)
		{
			if (FString(*Pair.Key).Equals(Key, ESearchCase::CaseSensitive))
			{
				return true;
			}
		}
		return false;
	};

	UTestPlayer* Player = NewObject<UTestPlayer>();
	Player->Profile.Bio = TEXT("nested");
	Player->Profile.Age = 3;

	{
		FPlayServDataTestAccess::RegisterStructMemberNames(*ProfilePath, ProfileRecased, UE_ARRAY_COUNT(ProfileRecased));
		FPlayServDataTestAccess::RegisterStructMemberNames(*InnerPath, InnerRecased, UE_ARRAY_COUNT(InnerRecased));
		ON_SCOPE_EXIT
		{
			FPlayServDataTestAccess::RegisterStructMemberNames(*ProfilePath, ProfileDeclared, UE_ARRAY_COUNT(ProfileDeclared));
			FPlayServDataTestAccess::RegisterStructMemberNames(*InnerPath, InnerDeclared, UE_ARRAY_COUNT(InnerDeclared));
		};

		const TSharedPtr<FJsonObject> Json = FPlayServDataTestAccess::SerializeToJson(Player);
		const TSharedPtr<FJsonObject>* Profile = nullptr;
		if (TestTrue(TEXT("the nested part serialized"), Json.IsValid() && Json->TryGetObjectField(TEXT("Profile"), Profile)))
		{
			TestTrue(TEXT("part member written with the table's name 'bio'"), HasKey(*Profile, TEXT("bio")));
			TestTrue(TEXT("part member written with the table's name 'age'"), HasKey(*Profile, TEXT("age")));
			TestFalse(TEXT("GetName() spelling 'Bio' is not on the wire"), HasKey(*Profile, TEXT("Bio")));
		}

		TSharedPtr<FJsonObject> Incoming = MakeShared<FJsonObject>();
		TSharedPtr<FJsonObject> IncomingProfile = MakeShared<FJsonObject>();
		IncomingProfile->SetStringField(TEXT("bio"), TEXT("read back"));
		IncomingProfile->SetNumberField(TEXT("age"), 9);
		Incoming->SetObjectField(TEXT("Profile"), IncomingProfile);
		UTestPlayer* Restored = NewObject<UTestPlayer>();
		FPlayServDataTestAccess::DeserializeFromJson(Incoming, Restored);
		TestEqual(TEXT("part member read by the table's name"), Restored->Profile.Bio, FString(TEXT("read back")));
		TestEqual(TEXT("second part member read by the table's name"), Restored->Profile.Age, 9);

		FRpcTestInner Inner;
		Inner.Label = TEXT("rpc");
		Inner.Weight = 4;
		FPlayServRPCParams Params;
		TestTrue(TEXT("cloud-function struct converts"), PlayServ::Code::StructToRpcParams(FRpcTestInner::StaticStruct(), &Inner, Params));
		const TSharedPtr<FJsonObject> ParamsJson = Params.ToJson();
		if (TestTrue(TEXT("cloud-function params exist"), ParamsJson.IsValid()))
		{
			TestTrue(TEXT("cloud-function member written with the table's name 'label'"), HasKey(ParamsJson, TEXT("label")));
			TestFalse(TEXT("GetName() spelling 'Label' is not on the wire"), HasKey(ParamsJson, TEXT("Label")));
		}

		TSharedPtr<FJsonObject> Reply = MakeShared<FJsonObject>();
		Reply->SetStringField(TEXT("label"), TEXT("reply"));
		Reply->SetNumberField(TEXT("weight"), 7);
		FRpcTestInner Parsed;
		TestTrue(TEXT("cloud-function reply converts"), PlayServ::Code::JsonObjectToStruct(Reply, FRpcTestInner::StaticStruct(), &Parsed));
		TestEqual(TEXT("cloud-function reply member read by the table's name"), Parsed.Label, FString(TEXT("reply")));
		TestEqual(TEXT("second cloud-function reply member read by the table's name"), Parsed.Weight, 7);
	}

	const TSharedPtr<FJsonObject> After = FPlayServDataTestAccess::SerializeToJson(Player);
	const TSharedPtr<FJsonObject>* AfterProfile = nullptr;
	if (TestTrue(TEXT("the nested part serializes after the restore"), After.IsValid() && After->TryGetObjectField(TEXT("Profile"), AfterProfile)))
	{
		TestTrue(TEXT("the declared names are back after the test ('Bio')"), HasKey(*AfterProfile, TEXT("Bio")));
		TestFalse(TEXT("no recased name survives the test ('bio')"), HasKey(*AfterProfile, TEXT("bio")));
	}

	return true;
}

#endif // !UE_BUILD_SHIPPING
