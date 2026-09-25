#include "PlayServTestCommon.h"
#include "PlayServTestAccess.h"
#include "Core/PlayServSubsystem.h"
#include "Auth/PlayServAuth.h"
#include "Data/PlayServData.h"
#include "PlayServ.h"
#include "TestEntities.h"
#include "UObject/StrongObjectPtr.h"

#if !UE_BUILD_SHIPPING

// ---------------------------------------------------------------------------
// PlayServ.Data.Crud.* — id semantics: record ids are SERVER-minted.
// An entity has an EMPTY id until its first Save completes; the save binds the server's rec_*
// id, readable via UPlayServData::GetRecordId. Every create step therefore captures the id in
// the Save callback (never at Create time), and later steps address by the captured id.
// There is no way to give an entity an id of the caller's choosing. Tests that need a stub for a
// reference bind a known id through FPlayServDataTestAccess, as the serializer does for loaded refs.
// ---------------------------------------------------------------------------

// A well-formed rec_* id the server never minted — the canonical "missing row" probe.
// (File-unique name: this module builds unity, like the per-file cleanup helpers.)
static const TCHAR* const CrudMissingRecId = TEXT("rec_00000000000000000000000000");

// Cleanup helper — delete all CRUD entity types then logout, in THREE PHASES. V2's relation
// links refuse deleting a still-referenced record with 409 in_use, and TestPlayer<->TestClan
// reference each other (Player.Clan / Clan.Players) — a cycle no delete ordering can unwind.
// Phase 0 breaks the cycle: clear every clan's Players array (a player's own outbound refs die
// with its row, inbound ones do not). Phase 1 deletes the referencing types (TestPlayer,
// TestInventory), phase 2 the referenced ones (TestClan, TestItem). 30s: DeleteAll is
// query-then-fan-out on V2, so a leaked table from a crashed prior run can take a while.
static void AddCrudCleanupStep(FAutomationTestBase* Test)
{
	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	Test->AddCommand(new FPlayServAsyncStep(Test, TEXT("Cleanup"), [bDone]()
	{
		auto StartWave2 = [bDone]()
		{
			TSharedPtr<int32> Wave2Remaining = MakeShared<int32>(2);
			auto OnWave2Done = [bDone, Wave2Remaining]()
			{
				if (--(*Wave2Remaining) == 0)
				{
					// Cleanup no longer signs out: the session it used to tear down is the one the
					// next test reuses, and tearing it down costs a whole new player (PSV-2659).
					*bDone = true;
				}
			};
			PlayServ::Data::DeleteAll<UTestClan>(FPlayServFilter::All(), FPlayServDeleteAllCallback::CreateLambda(
				[OnWave2Done](bool, const FPlayServDeleteAllResult&, const FPlayServError&) { OnWave2Done(); }));
			PlayServ::Data::DeleteAll<UTestItem>(FPlayServFilter::All(), FPlayServDeleteAllCallback::CreateLambda(
				[OnWave2Done](bool, const FPlayServDeleteAllResult&, const FPlayServError&) { OnWave2Done(); }));
		};

		auto StartWave1 = [StartWave2]()
		{
			TSharedPtr<int32> Wave1Remaining = MakeShared<int32>(2);
			auto OnWave1Done = [Wave1Remaining, StartWave2]()
			{
				if (--(*Wave1Remaining) == 0)
				{
					StartWave2();
				}
			};
			PlayServ::Data::DeleteAll<UTestPlayer>(FPlayServFilter::All(), FPlayServDeleteAllCallback::CreateLambda(
				[OnWave1Done](bool, const FPlayServDeleteAllResult&, const FPlayServError&) { OnWave1Done(); }));
			PlayServ::Data::DeleteAll<UTestInventory>(FPlayServFilter::All(), FPlayServDeleteAllCallback::CreateLambda(
				[OnWave1Done](bool, const FPlayServDeleteAllResult&, const FPlayServError&) { OnWave1Done(); }));
		};

		PlayServ::Data::LoadAll<UTestClan>(FPlayServFilter::None(),
			[StartWave1](bool bSuccess, TArray<UTestClan*> Clans, const FPlayServError&)
		{
			TArray<UTestClan*> ToClear;
			if (bSuccess)
			{
				for (UTestClan* Clan : Clans)
				{
					if (Clan != nullptr && Clan->Players.Num() > 0)
					{
						ToClear.Add(Clan);
					}
				}
			}
			if (ToClear.Num() == 0)
			{
				StartWave1();
				return;
			}

			// Root the clans for the duration of their Saves — LoadAll'ed entities are transient.
			TSharedPtr<TArray<TStrongObjectPtr<UTestClan>>> Held = MakeShared<TArray<TStrongObjectPtr<UTestClan>>>();
			TSharedPtr<int32> ClearRemaining = MakeShared<int32>(ToClear.Num());
			for (UTestClan* Clan : ToClear)
			{
				Held->Emplace(Clan);
				Clan->Players.Empty();
				PlayServ::Data::Save(Clan, FPlayServSimpleCallback::CreateLambda(
					[Held, ClearRemaining, StartWave1](bool, const FPlayServError&)
					{
						if (--(*ClearRemaining) == 0)
						{
							StartWave1();
						}
					}));
			}
		});
	}, bDone, 30.0f));
}

// ---------------------------------------------------------------------------
// CREATE
// ---------------------------------------------------------------------------

// PlayServ.Data.Crud.CreateServerMintedId (was CreateAutoId — client-side UUID v7 died with V2)
// PSCreate assigns NO id; the first PSSave creates the row and binds the server-minted rec_* id.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServCrudCreateServerMintedIdTest,
	"PlayServ.Data.Crud.CreateServerMintedId",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServCrudCreateServerMintedIdTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("crud-create-minted"));

	TSharedPtr<FString> IdAtCreate = MakeShared<FString>(TEXT("unset"));
	TSharedPtr<FString> IdAfterSave = MakeShared<FString>();
	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	TSharedPtr<bool> bOk = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("CreateAndSave"), [bDone, bOk, IdAtCreate, IdAfterSave]()
	{
		UTestPlayer* Player = PlayServ::Data::Create<UTestPlayer>();
		Player->Name = TEXT("ServerMintedIdPlayer");
		Player->Level = 10;
		*IdAtCreate = UPlayServData::GetRecordId(Player);

		PlayServ::Data::Save(Player, FPlayServSimpleCallback::CreateLambda([bDone, bOk, IdAfterSave, Player](bool bSuccess, const FPlayServError&)
		{
			*bOk = bSuccess;
			*IdAfterSave = UPlayServData::GetRecordId(Player);
			*bDone = true;
		}));
	}, bDone));

	AddCommand(new FPlayServAssertStep(this, [bOk, IdAtCreate, IdAfterSave](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("Id is EMPTY between PSCreate and the first PSSave"), (*IdAtCreate).IsEmpty());
		T->TestTrue(TEXT("PSSave succeeded"), *bOk);
		T->TestFalse(TEXT("First save bound a non-empty id"), (*IdAfterSave).IsEmpty());
		T->TestTrue(TEXT("Bound id is server-minted (rec_ prefix)"), (*IdAfterSave).StartsWith(TEXT("rec_")));
		T->AddInfo(FString::Printf(TEXT("Server-minted id: %s"), **IdAfterSave));
	}));

	AddCrudCleanupStep(this);
	return true;
}

// PlayServ.Data.Crud.CreateTakesNoId (was CreateCustomIdIgnored, then CreateCustomId)
// The platform mints every record id, so Create<T> takes none: passing one no longer compiles.
// An entity is id-less until its first Save — CreateServerMintedId covers the id that Save binds.

template<typename T>
constexpr bool CreateAcceptsAnId = requires(const FString& Id) { PlayServ::Data::Create<T>(Id); };
static_assert(!CreateAcceptsAnId<UTestPlayer>, "Create<T> takes no id: the platform mints record ids");

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServCrudCreateTakesNoIdTest,
	"PlayServ.Data.Crud.CreateTakesNoId",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServCrudCreateTakesNoIdTest::RunTest(const FString& Parameters)
{
	UTestPlayer* Player = PlayServ::Data::Create<UTestPlayer>();
	TestNotNull(TEXT("Create<T> returns an instance"), Player);
	TestTrue(TEXT("a created entity has no record id before its first Save"), PlayServ::Data::GetRecordId(Player).IsEmpty());
	return true;
}

// PlayServ.Data.Crud.CreateWithoutId
// NewObject without Create<T> — Save creates the row and binds a server id.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServCrudCreateWithoutIdTest,
	"PlayServ.Data.Crud.CreateWithoutId",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServCrudCreateWithoutIdTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("crud-create-noid"));

	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	TSharedPtr<bool> bSaved = MakeShared<bool>(false);
	TSharedPtr<FString> AssignedId = MakeShared<FString>();
	TSharedPtr<FString> SaveError = MakeShared<FString>();
	AddCommand(new FPlayServAsyncStep(this, TEXT("SaveWithoutId"), [bDone, bSaved, AssignedId, SaveError]()
	{
		UPlayServSubsystem* PS = UPlayServSubsystem::Get();
		UTestPlayer* Player = NewObject<UTestPlayer>(PS);
		Player->Name = TEXT("NoIdPlayer");
		Player->Level = 1;

		PlayServ::Data::Save(Player, FPlayServSimpleCallback::CreateLambda([bDone, bSaved, AssignedId, SaveError, Player](bool bSuccess, const FPlayServError& Error)
		{
			*bSaved = bSuccess;
			*AssignedId = PlayServ::Data::GetRecordId(Player);
			*SaveError = Error.Message;
			*bDone = true;
		}));
	}, bDone));

	AddCommand(new FPlayServAssertStep(this, [bSaved, AssignedId, SaveError](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("Save without ID succeeds (server mints the id)"), *bSaved);
		T->TestFalse(TEXT("Server-bound ID is non-empty"), AssignedId->IsEmpty());
		T->AddInfo(FString::Printf(TEXT("Server-bound ID: %s"), **AssignedId));
		if (!*bSaved)
		{
			T->AddError(FString::Printf(TEXT("Error: %s"), **SaveError));
		}
	}));

	AddCrudCleanupStep(this);
	return true;
}

// ---------------------------------------------------------------------------
// READ
// ---------------------------------------------------------------------------

// PlayServ.Data.Crud.ReadBasic
// Create, save, read back — verify fields match AND loaded object is a different pointer.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServCrudReadBasicTest,
	"PlayServ.Data.Crud.ReadBasic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServCrudReadBasicTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("crud-load-verify"));

	TSharedPtr<FString> EntityId = MakeShared<FString>();
	TSharedPtr<UPTRINT> OriginalAddr = MakeShared<UPTRINT>(0);
	TSharedPtr<bool> bCreateDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("Create"), [bCreateDone, EntityId, OriginalAddr]()
	{
		UTestPlayer* Player = PlayServ::Data::Create<UTestPlayer>();
		Player->Name = TEXT("LoadTarget");
		Player->Level = 25;
		Player->Score = 77.5f;
		*OriginalAddr = reinterpret_cast<UPTRINT>(Player);

		PlayServ::Data::Save(Player, FPlayServSimpleCallback::CreateLambda([bCreateDone, EntityId, Player](bool, const FPlayServError&)
		{
			*EntityId = UPlayServData::GetRecordId(Player);
			*bCreateDone = true;
		}));
	}, bCreateDone));

	TSharedPtr<bool> bLoadDone = MakeShared<bool>(false);
	TSharedPtr<bool> bLoadOk = MakeShared<bool>(false);
	TSharedPtr<bool> bFieldsMatch = MakeShared<bool>(false);
	TSharedPtr<UPTRINT> LoadedAddr = MakeShared<UPTRINT>(0);
	AddCommand(new FPlayServAsyncStep(this, TEXT("Load"), [bLoadDone, bLoadOk, bFieldsMatch, LoadedAddr, EntityId]()
	{
		PlayServ::Data::Load<UTestPlayer>(*EntityId, [bLoadDone, bLoadOk, bFieldsMatch, LoadedAddr](bool bSuccess, UTestPlayer* Result, const FPlayServError&)
		{
			*bLoadOk = bSuccess;
			if (bSuccess && Result)
			{
				*LoadedAddr = reinterpret_cast<UPTRINT>(Result);
				*bFieldsMatch = Result->Name == TEXT("LoadTarget")
					&& Result->Level == 25
					&& FMath::IsNearlyEqual(Result->Score, 77.5f, 0.01f);
			}
			*bLoadDone = true;
		});
	}, bLoadDone));

	AddCommand(new FPlayServAssertStep(this, [bLoadOk, bFieldsMatch, OriginalAddr, LoadedAddr](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("PSLoad succeeded"), *bLoadOk);
		T->TestTrue(TEXT("Fields match (Name, Level, Score)"), *bFieldsMatch);
		T->TestTrue(TEXT("Loaded object is different pointer than original"), *LoadedAddr != *OriginalAddr);
		T->AddInfo(FString::Printf(TEXT("Original ptr: 0x%p, Loaded ptr: 0x%p"), (void*)*OriginalAddr, (void*)*LoadedAddr));
	}));

	AddCrudCleanupStep(this);
	return true;
}

// PlayServ.Data.Crud.ReadBasicTypes
// Create entities with every supported basic field type, save, read back, verify each field.
// Covers: FString, int32, float, bool, double, int64, USTRUCT, enum, TArray<FString>, TArray<int32>.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServCrudReadBasicTypesTest,
	"PlayServ.Data.Crud.ReadBasicTypes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServCrudReadBasicTypesTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("crud-basictypes"));

	// Step 1: Create and save clan with array fields
	TSharedPtr<FString> ClanId = MakeShared<FString>();
	TSharedPtr<bool> bClanDone = MakeShared<bool>(false);
	TSharedPtr<bool> bClanOk = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("CreateClan"), [bClanDone, bClanOk, ClanId]()
	{
		UTestClan* Clan = PlayServ::Data::Create<UTestClan>();
		Clan->ClanName = TEXT("TestClan");
		Clan->Tag = TEXT("TC");
		Clan->MemberCount = 3;
		Clan->Tags = { TEXT("pvp"), TEXT("competitive"), TEXT("ranked") };
		Clan->MemberScores = { 10, 25, 42 };

		PlayServ::Data::Save(Clan, FPlayServSimpleCallback::CreateLambda([bClanDone, bClanOk, ClanId, Clan](bool bSuccess, const FPlayServError&)
		{
			*bClanOk = bSuccess;
			*ClanId = UPlayServData::GetRecordId(Clan);
			*bClanDone = true;
		}));
	}, bClanDone));

	// Step 2: Create and save player with all basic field types + stub ref to clan
	TSharedPtr<FString> PlayerId = MakeShared<FString>();
	TSharedPtr<bool> bPlayerDone = MakeShared<bool>(false);
	TSharedPtr<bool> bPlayerOk = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("CreatePlayer"), [bPlayerDone, bPlayerOk, PlayerId, ClanId]()
	{
		UPlayServSubsystem* PS = UPlayServSubsystem::Get();
		UTestPlayer* Player = PlayServ::Data::Create<UTestPlayer>();
		Player->Name = TEXT("AllTypesPlayer");
		Player->Level = 42;
		Player->Score = 99.5f;
		Player->bActive = true;
		Player->Rating = 3.141592653589793;
		Player->PlayTime = 9876543210LL;
		Player->Rank = ETestRank::Gold;
		Player->Profile.Bio = TEXT("Test bio with special chars: <>&\"'");
		Player->Profile.Age = 30;
		Player->Profile.bVerified = true;

		// Assign clan ref via stub — binding the KNOWN server id captured from the clan's save
		UTestClan* ClanRef = NewObject<UTestClan>(PS);
		FPlayServDataTestAccess::BindRecordId(ClanRef, *ClanId);
		Player->Clan = ClanRef;

		PlayServ::Data::Save(Player, FPlayServSimpleCallback::CreateLambda([bPlayerDone, bPlayerOk, PlayerId, Player](bool bSuccess, const FPlayServError&)
		{
			*bPlayerOk = bSuccess;
			*PlayerId = UPlayServData::GetRecordId(Player);
			*bPlayerDone = true;
		}));
	}, bPlayerDone));

	AddCommand(new FPlayServAssertStep(this, [bClanOk, bPlayerOk](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("Clan save succeeded"), *bClanOk);
		T->TestTrue(TEXT("Player save succeeded"), *bPlayerOk);
	}));

	// Step 3: Load clan via PSLoad and verify array fields
	TSharedPtr<bool> bClanLoadDone = MakeShared<bool>(false);
	TSharedPtr<bool> bClanLoadOk = MakeShared<bool>(false);
	TSharedPtr<bool> bTagsOk = MakeShared<bool>(false);
	TSharedPtr<bool> bScoresOk = MakeShared<bool>(false);
	TSharedPtr<FString> TagsInfo = MakeShared<FString>(TEXT("(not loaded)"));
	AddCommand(new FPlayServAsyncStep(this, TEXT("LoadClan"),
		[bClanLoadDone, bClanLoadOk, bTagsOk, bScoresOk, TagsInfo, ClanId]()
	{
		PlayServ::Data::Load<UTestClan>(*ClanId,
			[bClanLoadDone, bClanLoadOk, bTagsOk, bScoresOk, TagsInfo]
			(bool bSuccess, UTestClan* Clan, const FPlayServError&)
		{
			*bClanLoadOk = bSuccess;
			if (bSuccess && Clan)
			{
				*TagsInfo = FString::Printf(TEXT("Num=%d"), Clan->Tags.Num());
				for (int32 i = 0; i < Clan->Tags.Num(); ++i)
				{
					*TagsInfo += FString::Printf(TEXT(" [%d]=%s"), i, *Clan->Tags[i]);
				}

				*bTagsOk = Clan->Tags.Num() == 3
					&& Clan->Tags[0] == TEXT("pvp")
					&& Clan->Tags[1] == TEXT("competitive")
					&& Clan->Tags[2] == TEXT("ranked");

				*bScoresOk = Clan->MemberScores.Num() == 3
					&& Clan->MemberScores[0] == 10
					&& Clan->MemberScores[1] == 25
					&& Clan->MemberScores[2] == 42;
			}
			*bClanLoadDone = true;
		});
	}, bClanLoadDone));

	// Step 4: Load player and verify all basic field types
	TSharedPtr<bool> bPlayerLoadDone = MakeShared<bool>(false);
	TSharedPtr<bool> bNameOk = MakeShared<bool>(false);
	TSharedPtr<bool> bLevelOk = MakeShared<bool>(false);
	TSharedPtr<bool> bScoreOk = MakeShared<bool>(false);
	TSharedPtr<bool> bActiveOk = MakeShared<bool>(false);
	TSharedPtr<bool> bRatingOk = MakeShared<bool>(false);
	TSharedPtr<bool> bPlayTimeOk = MakeShared<bool>(false);
	TSharedPtr<bool> bRankOk = MakeShared<bool>(false);
	TSharedPtr<bool> bProfileBioOk = MakeShared<bool>(false);
	TSharedPtr<bool> bProfileAgeOk = MakeShared<bool>(false);
	TSharedPtr<bool> bProfileVerifiedOk = MakeShared<bool>(false);
	TSharedPtr<bool> bClanRefOk = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("LoadPlayer"),
		[bPlayerLoadDone, bNameOk, bLevelOk, bScoreOk, bActiveOk, bRatingOk, bPlayTimeOk,
		 bRankOk, bProfileBioOk, bProfileAgeOk, bProfileVerifiedOk, bClanRefOk,
		 PlayerId, ClanId]()
	{
		PlayServ::Data::Load<UTestPlayer>(*PlayerId,
			[bPlayerLoadDone, bNameOk, bLevelOk, bScoreOk, bActiveOk, bRatingOk, bPlayTimeOk,
			 bRankOk, bProfileBioOk, bProfileAgeOk, bProfileVerifiedOk, bClanRefOk,
			 ClanId]
			(bool bSuccess, UTestPlayer* P, const FPlayServError&)
		{
			if (bSuccess && P)
			{
				*bNameOk = P->Name == TEXT("AllTypesPlayer");
				*bLevelOk = P->Level == 42;
				*bScoreOk = FMath::IsNearlyEqual(P->Score, 99.5f, 0.01f);
				*bActiveOk = P->bActive == true;
				*bRatingOk = FMath::IsNearlyEqual(P->Rating, 3.141592653589793, 0.0000001);
				*bPlayTimeOk = P->PlayTime == 9876543210LL;
				*bRankOk = P->Rank == ETestRank::Gold;
				*bProfileBioOk = P->Profile.Bio == TEXT("Test bio with special chars: <>&\"'");
				*bProfileAgeOk = P->Profile.Age == 30;
				*bProfileVerifiedOk = P->Profile.bVerified == true;

				// Entity ref: should be a stub with correct ID but empty data
				if (P->Clan)
				{
					FString LoadedClanId = UPlayServData::GetRecordId(P->Clan);
					*bClanRefOk = LoadedClanId == *ClanId && P->Clan->ClanName.IsEmpty();
				}
			}
			*bPlayerLoadDone = true;
		});
	}, bPlayerLoadDone));

	AddCommand(new FPlayServAssertStep(this,
		[bClanLoadOk, bTagsOk, bScoresOk, TagsInfo,
		 bNameOk, bLevelOk, bScoreOk, bActiveOk, bRatingOk, bPlayTimeOk,
		 bRankOk, bProfileBioOk, bProfileAgeOk, bProfileVerifiedOk,
		 bClanRefOk](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("Clan load succeeded"), *bClanLoadOk);
		T->TestTrue(TEXT("TArray<FString> Tags roundtrip"), *bTagsOk);
		T->TestTrue(TEXT("TArray<int32> MemberScores roundtrip"), *bScoresOk);
		T->AddInfo(FString::Printf(TEXT("Tags: %s"), **TagsInfo));
		T->TestTrue(TEXT("FString Name roundtrip"), *bNameOk);
		T->TestTrue(TEXT("int32 Level roundtrip"), *bLevelOk);
		T->TestTrue(TEXT("float Score roundtrip"), *bScoreOk);
		T->TestTrue(TEXT("bool bActive roundtrip"), *bActiveOk);
		T->TestTrue(TEXT("double Rating roundtrip"), *bRatingOk);
		T->TestTrue(TEXT("int64 PlayTime roundtrip"), *bPlayTimeOk);
		T->TestTrue(TEXT("ETestRank Rank roundtrip (Gold)"), *bRankOk);
		T->TestTrue(TEXT("USTRUCT Profile.Bio roundtrip"), *bProfileBioOk);
		T->TestTrue(TEXT("USTRUCT Profile.Age roundtrip"), *bProfileAgeOk);
		T->TestTrue(TEXT("USTRUCT Profile.bVerified roundtrip"), *bProfileVerifiedOk);
		T->TestTrue(TEXT("Entity ref Clan (stub with ID, empty data)"), *bClanRefOk);
	}));

	AddCrudCleanupStep(this);
	return true;
}

// PlayServ.Data.Crud.ReadRefTypes
// Entity references: direct ref + populate, array refs + PSPopulateArray,
// refs inside value object (FInventorySlot) + manual populate, non-persistent ref skip.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServCrudReadRefTypesTest,
	"PlayServ.Data.Crud.ReadRefTypes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServCrudReadRefTypesTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("crud-reftypes"));

	// --- Part 1: Direct entity ref + populate ---

	// Create and save a clan
	TSharedPtr<FString> ClanId = MakeShared<FString>();
	TSharedPtr<bool> bClanDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("CreateClan"), [bClanDone, ClanId]()
	{
		UTestClan* Clan = PlayServ::Data::Create<UTestClan>();
		Clan->ClanName = TEXT("RefWarriors");
		Clan->Tag = TEXT("RW");
		Clan->MemberCount = 10;

		PlayServ::Data::Save(Clan, FPlayServSimpleCallback::CreateLambda([bClanDone, ClanId, Clan](bool, const FPlayServError&)
		{
			*ClanId = UPlayServData::GetRecordId(Clan);
			*bClanDone = true;
		}));
	}, bClanDone));

	// Create player with clan ref (stub) and save
	TSharedPtr<FString> PlayerId = MakeShared<FString>();
	TSharedPtr<bool> bPlayerDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("CreatePlayerWithRef"), [bPlayerDone, PlayerId, ClanId]()
	{
		UPlayServSubsystem* PS = UPlayServSubsystem::Get();
		UTestPlayer* Player = PlayServ::Data::Create<UTestPlayer>();
		Player->Name = TEXT("RefTestPlayer");
		Player->Level = 10;

		UTestClan* ClanStub = NewObject<UTestClan>(PS);
		FPlayServDataTestAccess::BindRecordId(ClanStub, *ClanId);
		Player->Clan = ClanStub;

		PlayServ::Data::Save(Player, FPlayServSimpleCallback::CreateLambda([bPlayerDone, PlayerId, Player](bool, const FPlayServError&)
		{
			*PlayerId = UPlayServData::GetRecordId(Player);
			*bPlayerDone = true;
		}));
	}, bPlayerDone));

	// Load player, verify clan is stub, populate, verify full data
	TSharedPtr<bool> bDirectRefDone = MakeShared<bool>(false);
	TSharedPtr<bool> bIsStub = MakeShared<bool>(false);
	TSharedPtr<bool> bPopulateOk = MakeShared<bool>(false);
	TSharedPtr<bool> bPopulateFieldsOk = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("VerifyDirectRef"),
		[bDirectRefDone, bIsStub, bPopulateOk, bPopulateFieldsOk, PlayerId, ClanId]()
	{
		PlayServ::Data::Load<UTestPlayer>(*PlayerId,
			[bDirectRefDone, bIsStub, bPopulateOk, bPopulateFieldsOk, ClanId]
			(bool bLoad, UTestPlayer* Player, const FPlayServError&)
		{
			if (!bLoad || !Player || !Player->Clan)
			{
				*bDirectRefDone = true;
				return;
			}

			FString StubId = UPlayServData::GetRecordId(Player->Clan);
			*bIsStub = !StubId.IsEmpty() && StubId == *ClanId && Player->Clan->ClanName.IsEmpty();

			PlayServ::Data::Populate(Player->Clan,
				FPlayServSimpleCallback::CreateLambda([bDirectRefDone, bPopulateOk, bPopulateFieldsOk,
				 WeakClan = TWeakObjectPtr<UTestClan>(Player->Clan)]
				(bool bSuccess, const FPlayServError&)
			{
				*bPopulateOk = bSuccess;
				UTestClan* Clan = WeakClan.Get();
				if (bSuccess && Clan)
				{
					*bPopulateFieldsOk = Clan->ClanName == TEXT("RefWarriors")
						&& Clan->Tag == TEXT("RW")
						&& Clan->MemberCount == 10;
				}
				*bDirectRefDone = true;
			}));
		});
	}, bDirectRefDone, 3.0f));

	// --- Part 2: Array of entity refs + PSPopulateArray ---

	// Create 3 players to use as array entries
	TSharedPtr<TArray<FString>> PlayerIds = MakeShared<TArray<FString>>();
	TSharedPtr<bool> bArrayPlayersDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("CreateArrayPlayers"), [bArrayPlayersDone, PlayerIds]()
	{
		UPlayServSubsystem* PS = UPlayServSubsystem::Get();
		TArray<UObject*> Batch;

		for (int32 i = 0; i < 3; ++i)
		{
			UTestPlayer* P = PlayServ::Data::Create<UTestPlayer>();
			P->Name = FString::Printf(TEXT("ArrayPlayer_%d"), i);
			P->Level = (i + 1) * 10;
			Batch.Add(P);
		}

		// Ids exist only after the batch create completes — capture them in creation order.
		PlayServ::Data::BulkSave(Batch, FPlayServSimpleCallback::CreateLambda(
			[bArrayPlayersDone, PlayerIds, Batch](bool, const FPlayServError&)
		{
			for (UObject* Obj : Batch)
			{
				PlayerIds->Add(UPlayServData::GetRecordId(Obj));
			}
			*bArrayPlayersDone = true;
		}));
	}, bArrayPlayersDone));

	// Create clan with Players array as stubs
	TSharedPtr<FString> ArrayClanId = MakeShared<FString>();
	TSharedPtr<bool> bArrayClanDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("CreateClanWithArray"),
		[bArrayClanDone, ArrayClanId, PlayerIds]()
	{
		UPlayServSubsystem* PS = UPlayServSubsystem::Get();
		UTestClan* Clan = PlayServ::Data::Create<UTestClan>();
		Clan->ClanName = TEXT("ArrayClan");
		Clan->MemberCount = 3;

		for (const FString& PId : *PlayerIds)
		{
			UTestPlayer* Stub = NewObject<UTestPlayer>(PS);
			FPlayServDataTestAccess::BindRecordId(Stub, PId);
			Clan->Players.Add(Stub);
		}

		PlayServ::Data::Save(Clan, FPlayServSimpleCallback::CreateLambda([bArrayClanDone, ArrayClanId, Clan](bool, const FPlayServError&)
		{
			*ArrayClanId = UPlayServData::GetRecordId(Clan);
			*bArrayClanDone = true;
		}));
	}, bArrayClanDone));

	// Load clan, verify stubs, PSPopulateArray, verify populated
	TSharedPtr<bool> bArrayRefDone = MakeShared<bool>(false);
	TSharedPtr<bool> bArrayStubsOk = MakeShared<bool>(false);
	TSharedPtr<bool> bArrayPopulateOk = MakeShared<bool>(false);
	TSharedPtr<bool> bArrayFieldsOk = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("VerifyArrayRefs"),
		[bArrayRefDone, bArrayStubsOk, bArrayPopulateOk, bArrayFieldsOk, ArrayClanId, PlayerIds]()
	{
		PlayServ::Data::Load<UTestClan>(*ArrayClanId,
			[bArrayRefDone, bArrayStubsOk, bArrayPopulateOk, bArrayFieldsOk, PlayerIds]
			(bool bLoad, UTestClan* Clan, const FPlayServError&)
		{
			if (!bLoad || !Clan)
			{
				*bArrayRefDone = true;
				return;
			}

			// Verify stubs — IDs set, data empty
			bool bAllStubs = Clan->Players.Num() == 3;
			for (int32 i = 0; i < Clan->Players.Num() && bAllStubs; ++i)
			{
				if (!Clan->Players[i])
				{
					bAllStubs = false;
					continue;
				}
				FString PId = UPlayServData::GetRecordId(Clan->Players[i]);
				bAllStubs = bAllStubs && !PId.IsEmpty() && Clan->Players[i]->Name.IsEmpty();
			}
			*bArrayStubsOk = bAllStubs;

			// PSPopulateArray
			TWeakObjectPtr<UTestClan> WeakClan(Clan);
			PlayServ::Data::PopulateArray(Clan, TEXT("Players"), FPlayServSimpleCallback::CreateLambda(
				[bArrayRefDone, bArrayPopulateOk, bArrayFieldsOk, WeakClan]
				(bool bSuccess, const FPlayServError&)
			{
				*bArrayPopulateOk = bSuccess;
				UTestClan* C = WeakClan.Get();
				if (bSuccess && C)
				{
					bool bFieldsOk = true;
					for (int32 i = 0; i < C->Players.Num(); ++i)
					{
						if (!C->Players[i])
						{
							bFieldsOk = false;
							continue;
						}
						FString ExpectedName = FString::Printf(TEXT("ArrayPlayer_%d"), i);
						bFieldsOk = bFieldsOk
							&& C->Players[i]->Name == ExpectedName
							&& C->Players[i]->Level == (i + 1) * 10;
					}
					*bArrayFieldsOk = bFieldsOk;
				}
				*bArrayRefDone = true;
			}));
		});
	}, bArrayRefDone, 5.0f));

	// --- Part 3: Entity refs inside value object (FInventorySlot) ---

	// Create 2 items
	TSharedPtr<TArray<FString>> ItemIds = MakeShared<TArray<FString>>();
	TSharedPtr<bool> bItemsDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("CreateItems"), [bItemsDone, ItemIds]()
	{
		UPlayServSubsystem* PS = UPlayServSubsystem::Get();
		TArray<UObject*> Batch;

		UTestItem* Sword = PlayServ::Data::Create<UTestItem>();
		Sword->ItemName = TEXT("Excalibur");
		Sword->Power = 100;
		Sword->Rarity = ETestRank::Gold;
		Batch.Add(Sword);

		UTestItem* Shield = PlayServ::Data::Create<UTestItem>();
		Shield->ItemName = TEXT("Aegis");
		Shield->Power = 80;
		Shield->Rarity = ETestRank::Silver;
		Batch.Add(Shield);

		// Capture the server ids in slot order (0 = sword, 1 = shield) once the creates land.
		PlayServ::Data::BulkSave(Batch, FPlayServSimpleCallback::CreateLambda(
			[bItemsDone, ItemIds, Sword, Shield](bool, const FPlayServError&)
		{
			ItemIds->Add(UPlayServData::GetRecordId(Sword));
			ItemIds->Add(UPlayServData::GetRecordId(Shield));
			*bItemsDone = true;
		}));
	}, bItemsDone));

	// Create inventory with FInventorySlot entries referencing items as stubs
	TSharedPtr<FString> InventoryId = MakeShared<FString>();
	TSharedPtr<bool> bInvDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("CreateInventory"), [bInvDone, InventoryId, ItemIds]()
	{
		UPlayServSubsystem* PS = UPlayServSubsystem::Get();
		UTestInventory* Inv = PlayServ::Data::Create<UTestInventory>();
		Inv->MaxSlots = 20;

		FInventorySlot Slot0;
		UTestItem* SwordStub = NewObject<UTestItem>(PS);
		FPlayServDataTestAccess::BindRecordId(SwordStub, (*ItemIds)[0]);
		Slot0.Item = SwordStub;
		Slot0.Count = 1;
		Slot0.Notes = TEXT("Main weapon");
		Inv->Slots.Add(Slot0);

		FInventorySlot Slot1;
		UTestItem* ShieldStub = NewObject<UTestItem>(PS);
		FPlayServDataTestAccess::BindRecordId(ShieldStub, (*ItemIds)[1]);
		Slot1.Item = ShieldStub;
		Slot1.Count = 3;
		Slot1.Notes = TEXT("Backup shields");
		Inv->Slots.Add(Slot1);

		PlayServ::Data::Save(Inv, FPlayServSimpleCallback::CreateLambda([bInvDone, InventoryId, Inv](bool, const FPlayServError&)
		{
			*InventoryId = UPlayServData::GetRecordId(Inv);
			*bInvDone = true;
		}));
	}, bInvDone));

	// Load inventory, verify slots + stubs, manually populate item stubs
	TSharedPtr<bool> bInvRefDone = MakeShared<bool>(false);
	TSharedPtr<bool> bSlotsOk = MakeShared<bool>(false);
	TSharedPtr<bool> bSlotDataOk = MakeShared<bool>(false);
	TSharedPtr<bool> bSlotStubsOk = MakeShared<bool>(false);
	TSharedPtr<bool> bSlotPopulateOk = MakeShared<bool>(false);
	TSharedPtr<bool> bSlotItemsOk = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("VerifyInventoryRefs"),
		[bInvRefDone, bSlotsOk, bSlotDataOk, bSlotStubsOk, bSlotPopulateOk, bSlotItemsOk,
		 InventoryId, ItemIds]()
	{
		PlayServ::Data::Load<UTestInventory>(*InventoryId,
			[bInvRefDone, bSlotsOk, bSlotDataOk, bSlotStubsOk, bSlotPopulateOk, bSlotItemsOk,
			 ItemIds]
			(bool bLoad, UTestInventory* Inv, const FPlayServError&)
		{
			if (!bLoad || !Inv)
			{
				*bInvRefDone = true;
				return;
			}

			*bSlotsOk = Inv->Slots.Num() == 2 && Inv->MaxSlots == 20;

			// Verify slot scalar data
			if (Inv->Slots.Num() == 2)
			{
				*bSlotDataOk = Inv->Slots[0].Count == 1
					&& Inv->Slots[0].Notes == TEXT("Main weapon")
					&& Inv->Slots[1].Count == 3
					&& Inv->Slots[1].Notes == TEXT("Backup shields");

				// Verify item refs are stubs
				bool bStubs = true;
				for (int32 i = 0; i < 2; ++i)
				{
					if (!Inv->Slots[i].Item)
					{
						bStubs = false;
						continue;
					}
					FString ItemId = UPlayServData::GetRecordId(Inv->Slots[i].Item);
					bStubs = bStubs && ItemId == (*ItemIds)[i] && Inv->Slots[i].Item->ItemName.IsEmpty();
				}
				*bSlotStubsOk = bStubs;

				// Populate both item stubs manually
				TSharedPtr<int32> PopRemaining = MakeShared<int32>(2);
				TSharedPtr<bool> bAllPopOk = MakeShared<bool>(true);
				for (int32 i = 0; i < 2; ++i)
				{
					if (!Inv->Slots[i].Item)
					{
						--(*PopRemaining);
						continue;
					}
					TWeakObjectPtr<UTestItem> WeakItem(Inv->Slots[i].Item);
					PlayServ::Data::Populate(Inv->Slots[i].Item,
						FPlayServSimpleCallback::CreateLambda([bInvRefDone, bSlotPopulateOk, bSlotItemsOk, bAllPopOk,
						 PopRemaining, WeakItem, i, ItemIds, WeakInv = TWeakObjectPtr<UTestInventory>(Inv)]
						(bool bSuccess, const FPlayServError&)
					{
						if (!bSuccess) { *bAllPopOk = false; }
						if (--(*PopRemaining) == 0)
						{
							*bSlotPopulateOk = *bAllPopOk;

							UTestInventory* InvCheck = WeakInv.Get();
							if (InvCheck && InvCheck->Slots.Num() == 2)
							{
								bool bItems = true;
								if (InvCheck->Slots[0].Item)
								{
									bItems = bItems
										&& InvCheck->Slots[0].Item->ItemName == TEXT("Excalibur")
										&& InvCheck->Slots[0].Item->Power == 100
										&& InvCheck->Slots[0].Item->Rarity == ETestRank::Gold;
								}
								else { bItems = false; }
								if (InvCheck->Slots[1].Item)
								{
									bItems = bItems
										&& InvCheck->Slots[1].Item->ItemName == TEXT("Aegis")
										&& InvCheck->Slots[1].Item->Power == 80
										&& InvCheck->Slots[1].Item->Rarity == ETestRank::Silver;
								}
								else { bItems = false; }
								*bSlotItemsOk = bItems;
							}

							*bInvRefDone = true;
						}
					}));
				}
			}
			else
			{
				*bInvRefDone = true;
			}
		});
	}, bInvRefDone, 5.0f));

	// --- Part 4: Non-persistent ref skip ---
	// Save player with Transient set to a non-persistent UObject, load, verify null

	TSharedPtr<FString> TransientPlayerId = MakeShared<FString>();
	TSharedPtr<bool> bTransientSaveDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("CreateTransientRef"), [bTransientSaveDone, TransientPlayerId]()
	{
		UPlayServSubsystem* PS = UPlayServSubsystem::Get();
		UTestPlayer* Player = PlayServ::Data::Create<UTestPlayer>();
		Player->Name = TEXT("TransientRefPlayer");
		Player->Level = 1;

		// Set a non-persistent ref — serializer should skip it
		UTestTransient* Trans = NewObject<UTestTransient>(PS);
		Trans->Label = TEXT("ShouldBeSkipped");
		Player->Transient = Trans;

		PlayServ::Data::Save(Player, FPlayServSimpleCallback::CreateLambda([bTransientSaveDone, TransientPlayerId, Player](bool, const FPlayServError&)
		{
			*TransientPlayerId = UPlayServData::GetRecordId(Player);
			*bTransientSaveDone = true;
		}));
	}, bTransientSaveDone));

	TSharedPtr<bool> bTransientLoadDone = MakeShared<bool>(false);
	TSharedPtr<bool> bTransientIsNull = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("VerifyTransientNull"),
		[bTransientLoadDone, bTransientIsNull, TransientPlayerId]()
	{
		PlayServ::Data::Load<UTestPlayer>(*TransientPlayerId,
			[bTransientLoadDone, bTransientIsNull]
			(bool bLoad, UTestPlayer* P, const FPlayServError&)
		{
			if (bLoad && P)
			{
				*bTransientIsNull = P->Transient == nullptr;
			}
			*bTransientLoadDone = true;
		});
	}, bTransientLoadDone));

	AddCommand(new FPlayServAssertStep(this,
		[bIsStub, bPopulateOk, bPopulateFieldsOk,
		 bArrayStubsOk, bArrayPopulateOk, bArrayFieldsOk,
		 bSlotsOk, bSlotDataOk, bSlotStubsOk, bSlotPopulateOk, bSlotItemsOk,
		 bTransientIsNull](FAutomationTestBase* T)
	{
		// Part 1: Direct ref
		T->TestTrue(TEXT("Direct ref is stub (ID set, data empty)"), *bIsStub);
		T->TestTrue(TEXT("PSPopulate on direct ref succeeded"), *bPopulateOk);
		T->TestTrue(TEXT("Direct ref fields match after populate"), *bPopulateFieldsOk);

		// Part 2: Array refs
		T->TestTrue(TEXT("Array refs are stubs (3 players, IDs set, data empty)"), *bArrayStubsOk);
		T->TestTrue(TEXT("PSPopulateArray succeeded"), *bArrayPopulateOk);
		T->TestTrue(TEXT("Array ref fields match after PSPopulateArray"), *bArrayFieldsOk);

		// Part 3: Refs inside value object
		T->TestTrue(TEXT("Inventory has 2 slots, MaxSlots=20"), *bSlotsOk);
		T->TestTrue(TEXT("Slot scalar data (Count, Notes) roundtrip"), *bSlotDataOk);
		T->TestTrue(TEXT("Slot item refs are stubs (IDs set, names empty)"), *bSlotStubsOk);
		T->TestTrue(TEXT("Manual PSPopulate on slot items succeeded"), *bSlotPopulateOk);
		T->TestTrue(TEXT("Slot items have full data after populate"), *bSlotItemsOk);

		// Part 4: Non-persistent ref skip
		T->TestTrue(TEXT("Non-persistent TObjectPtr<UObject> is null after load"), *bTransientIsNull);
	}));

	AddCrudCleanupStep(this);
	return true;
}

// ---------------------------------------------------------------------------
// UPDATE
// ---------------------------------------------------------------------------

// PlayServ.Data.Crud.UpdateBasicTypes
// Create with initial values, load, modify all basic fields, save, load again, verify new values.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServCrudUpdateBasicTypesTest,
	"PlayServ.Data.Crud.UpdateBasicTypes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServCrudUpdateBasicTypesTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("crud-update-basic"));

	// Create with initial values
	TSharedPtr<FString> EntityId = MakeShared<FString>();
	TSharedPtr<bool> bCreateDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("Create"), [bCreateDone, EntityId]()
	{
		UTestPlayer* Player = PlayServ::Data::Create<UTestPlayer>();
		Player->Name = TEXT("BeforeUpdate");
		Player->Level = 1;
		Player->Score = 10.0f;
		Player->bActive = false;
		Player->Rating = 1.0;
		Player->PlayTime = 100;
		Player->Rank = ETestRank::Bronze;
		Player->Profile.Bio = TEXT("Original bio");
		Player->Profile.Age = 20;
		Player->Profile.bVerified = false;

		PlayServ::Data::Save(Player, FPlayServSimpleCallback::CreateLambda([bCreateDone, EntityId, Player](bool, const FPlayServError&)
		{
			*EntityId = UPlayServData::GetRecordId(Player);
			*bCreateDone = true;
		}));
	}, bCreateDone));

	// Load and modify all fields
	TSharedPtr<bool> bModifyDone = MakeShared<bool>(false);
	TSharedPtr<bool> bModifyOk = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("Modify"), [bModifyDone, bModifyOk, EntityId]()
	{
		PlayServ::Data::Load<UTestPlayer>(*EntityId, [bModifyDone, bModifyOk](bool bLoad, UTestPlayer* Player, const FPlayServError&)
		{
			if (!bLoad || !Player)
			{
				*bModifyDone = true;
				return;
			}
			Player->Name = TEXT("AfterUpdate");
			Player->Level = 99;
			Player->Score = 999.9f;
			Player->bActive = true;
			Player->Rating = 9.99;
			Player->PlayTime = 9999999;
			Player->Rank = ETestRank::Gold;
			Player->Profile.Bio = TEXT("Updated bio");
			Player->Profile.Age = 35;
			Player->Profile.bVerified = true;

			PlayServ::Data::Save(Player,FPlayServSimpleCallback::CreateLambda([bModifyDone, bModifyOk](bool bSave, const FPlayServError&)
			{
				*bModifyOk = bSave;
				*bModifyDone = true;
			}));
		});
	}, bModifyDone));

	// Load and verify all fields have new values
	TSharedPtr<bool> bVerifyDone = MakeShared<bool>(false);
	TSharedPtr<bool> bNameOk = MakeShared<bool>(false);
	TSharedPtr<bool> bLevelOk = MakeShared<bool>(false);
	TSharedPtr<bool> bScoreOk = MakeShared<bool>(false);
	TSharedPtr<bool> bActiveOk = MakeShared<bool>(false);
	TSharedPtr<bool> bRatingOk = MakeShared<bool>(false);
	TSharedPtr<bool> bPlayTimeOk = MakeShared<bool>(false);
	TSharedPtr<bool> bRankOk = MakeShared<bool>(false);
	TSharedPtr<bool> bBioOk = MakeShared<bool>(false);
	TSharedPtr<bool> bAgeOk = MakeShared<bool>(false);
	TSharedPtr<bool> bVerifiedOk = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("Verify"),
		[bVerifyDone, bNameOk, bLevelOk, bScoreOk, bActiveOk, bRatingOk, bPlayTimeOk,
		 bRankOk, bBioOk, bAgeOk, bVerifiedOk, EntityId]()
	{
		PlayServ::Data::Load<UTestPlayer>(*EntityId,
			[bVerifyDone, bNameOk, bLevelOk, bScoreOk, bActiveOk, bRatingOk, bPlayTimeOk,
			 bRankOk, bBioOk, bAgeOk, bVerifiedOk]
			(bool bLoad, UTestPlayer* P, const FPlayServError&)
		{
			if (bLoad && P)
			{
				*bNameOk = P->Name == TEXT("AfterUpdate");
				*bLevelOk = P->Level == 99;
				*bScoreOk = FMath::IsNearlyEqual(P->Score, 999.9f, 0.1f);
				*bActiveOk = P->bActive == true;
				*bRatingOk = FMath::IsNearlyEqual(P->Rating, 9.99, 0.001);
				*bPlayTimeOk = P->PlayTime == 9999999;
				*bRankOk = P->Rank == ETestRank::Gold;
				*bBioOk = P->Profile.Bio == TEXT("Updated bio");
				*bAgeOk = P->Profile.Age == 35;
				*bVerifiedOk = P->Profile.bVerified == true;
			}
			*bVerifyDone = true;
		});
	}, bVerifyDone));

	AddCommand(new FPlayServAssertStep(this,
		[bModifyOk, bNameOk, bLevelOk, bScoreOk, bActiveOk, bRatingOk, bPlayTimeOk,
		 bRankOk, bBioOk, bAgeOk, bVerifiedOk](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("Delta save succeeded"), *bModifyOk);
		T->TestTrue(TEXT("FString Name updated"), *bNameOk);
		T->TestTrue(TEXT("int32 Level updated"), *bLevelOk);
		T->TestTrue(TEXT("float Score updated"), *bScoreOk);
		T->TestTrue(TEXT("bool bActive updated"), *bActiveOk);
		T->TestTrue(TEXT("double Rating updated"), *bRatingOk);
		T->TestTrue(TEXT("int64 PlayTime updated"), *bPlayTimeOk);
		T->TestTrue(TEXT("ETestRank Rank updated (Bronze -> Gold)"), *bRankOk);
		T->TestTrue(TEXT("USTRUCT Profile.Bio updated"), *bBioOk);
		T->TestTrue(TEXT("USTRUCT Profile.Age updated"), *bAgeOk);
		T->TestTrue(TEXT("USTRUCT Profile.bVerified updated"), *bVerifiedOk);
	}));

	AddCrudCleanupStep(this);
	return true;
}

// PlayServ.Data.Crud.UpdateRefTypes
// Modify entity references and re-save: swap clan ref, modify array refs, update inventory slots.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServCrudUpdateRefTypesTest,
	"PlayServ.Data.Crud.UpdateRefTypes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServCrudUpdateRefTypesTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("crud-update-refs"));

	// Create two clans
	TSharedPtr<FString> Clan1Id = MakeShared<FString>();
	TSharedPtr<FString> Clan2Id = MakeShared<FString>();
	TSharedPtr<bool> bClansDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("CreateClans"), [bClansDone, Clan1Id, Clan2Id]()
	{
		UPlayServSubsystem* PS = UPlayServSubsystem::Get();
		TArray<UObject*> Batch;

		UTestClan* C1 = PlayServ::Data::Create<UTestClan>();
		C1->ClanName = TEXT("OriginalClan");
		Batch.Add(C1);

		UTestClan* C2 = PlayServ::Data::Create<UTestClan>();
		C2->ClanName = TEXT("NewClan");
		Batch.Add(C2);

		PlayServ::Data::BulkSave(Batch, FPlayServSimpleCallback::CreateLambda(
			[bClansDone, Clan1Id, Clan2Id, C1, C2](bool, const FPlayServError&)
		{
			*Clan1Id = UPlayServData::GetRecordId(C1);
			*Clan2Id = UPlayServData::GetRecordId(C2);
			*bClansDone = true;
		}));
	}, bClansDone));

	// Create player with clan1 ref
	TSharedPtr<FString> PlayerId = MakeShared<FString>();
	TSharedPtr<bool> bPlayerDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("CreatePlayer"), [bPlayerDone, PlayerId, Clan1Id]()
	{
		UPlayServSubsystem* PS = UPlayServSubsystem::Get();
		UTestPlayer* Player = PlayServ::Data::Create<UTestPlayer>();
		Player->Name = TEXT("SwapRefPlayer");
		Player->Level = 5;

		UTestClan* ClanStub = NewObject<UTestClan>(PS);
		FPlayServDataTestAccess::BindRecordId(ClanStub, *Clan1Id);
		Player->Clan = ClanStub;

		PlayServ::Data::Save(Player, FPlayServSimpleCallback::CreateLambda([bPlayerDone, PlayerId, Player](bool, const FPlayServError&)
		{
			*PlayerId = UPlayServData::GetRecordId(Player);
			*bPlayerDone = true;
		}));
	}, bPlayerDone));

	// Load player, swap clan ref to clan2, save
	TSharedPtr<bool> bSwapDone = MakeShared<bool>(false);
	TSharedPtr<bool> bSwapOk = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("SwapRef"), [bSwapDone, bSwapOk, PlayerId, Clan2Id]()
	{
		PlayServ::Data::Load<UTestPlayer>(*PlayerId, [bSwapDone, bSwapOk, Clan2Id](bool bLoad, UTestPlayer* Player, const FPlayServError&)
		{
			if (!bLoad || !Player)
			{
				*bSwapDone = true;
				return;
			}
			UTestClan* NewClanStub = NewObject<UTestClan>(Player);
			FPlayServDataTestAccess::BindRecordId(NewClanStub, *Clan2Id);
			Player->Clan = NewClanStub;

			PlayServ::Data::Save(Player,FPlayServSimpleCallback::CreateLambda([bSwapDone, bSwapOk](bool bSave, const FPlayServError&)
			{
				*bSwapOk = bSave;
				*bSwapDone = true;
			}));
		});
	}, bSwapDone));

	// Load again and verify the ref was swapped
	TSharedPtr<bool> bVerifyDone = MakeShared<bool>(false);
	TSharedPtr<bool> bRefSwapped = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("VerifySwap"), [bVerifyDone, bRefSwapped, PlayerId, Clan2Id]()
	{
		PlayServ::Data::Load<UTestPlayer>(*PlayerId, [bVerifyDone, bRefSwapped, Clan2Id](bool bLoad, UTestPlayer* Player, const FPlayServError&)
		{
			if (bLoad && Player && Player->Clan)
			{
				FString LoadedClanId = UPlayServData::GetRecordId(Player->Clan);
				*bRefSwapped = LoadedClanId == *Clan2Id;
			}
			*bVerifyDone = true;
		});
	}, bVerifyDone));

	// --- Part 2: Update inventory slot (swap item ref, change count) ---

	// Create 2 items
	TSharedPtr<FString> Item1Id = MakeShared<FString>();
	TSharedPtr<FString> Item2Id = MakeShared<FString>();
	TSharedPtr<bool> bItemsDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("CreateItems"), [bItemsDone, Item1Id, Item2Id]()
	{
		UPlayServSubsystem* PS = UPlayServSubsystem::Get();
		TArray<UObject*> Batch;

		UTestItem* I1 = PlayServ::Data::Create<UTestItem>();
		I1->ItemName = TEXT("OrigItem");
		I1->Power = 10;
		Batch.Add(I1);

		UTestItem* I2 = PlayServ::Data::Create<UTestItem>();
		I2->ItemName = TEXT("SwapItem");
		I2->Power = 50;
		Batch.Add(I2);

		PlayServ::Data::BulkSave(Batch, FPlayServSimpleCallback::CreateLambda(
			[bItemsDone, Item1Id, Item2Id, I1, I2](bool, const FPlayServError&)
		{
			*Item1Id = UPlayServData::GetRecordId(I1);
			*Item2Id = UPlayServData::GetRecordId(I2);
			*bItemsDone = true;
		}));
	}, bItemsDone));

	// Create inventory with 1 slot referencing item1
	TSharedPtr<FString> InvId = MakeShared<FString>();
	TSharedPtr<bool> bInvDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("CreateInventory"), [bInvDone, InvId, Item1Id]()
	{
		UPlayServSubsystem* PS = UPlayServSubsystem::Get();
		UTestInventory* Inv = PlayServ::Data::Create<UTestInventory>();

		FInventorySlot Slot;
		UTestItem* ItemStub = NewObject<UTestItem>(PS);
		FPlayServDataTestAccess::BindRecordId(ItemStub, *Item1Id);
		Slot.Item = ItemStub;
		Slot.Count = 1;
		Slot.Notes = TEXT("Original");
		Inv->Slots.Add(Slot);

		PlayServ::Data::Save(Inv, FPlayServSimpleCallback::CreateLambda([bInvDone, InvId, Inv](bool, const FPlayServError&)
		{
			*InvId = UPlayServData::GetRecordId(Inv);
			*bInvDone = true;
		}));
	}, bInvDone));

	// Load inventory, swap item ref and change count, save
	TSharedPtr<bool> bInvUpdateDone = MakeShared<bool>(false);
	TSharedPtr<bool> bInvUpdateOk = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("UpdateInventory"), [bInvUpdateDone, bInvUpdateOk, InvId, Item2Id]()
	{
		PlayServ::Data::Load<UTestInventory>(*InvId, [bInvUpdateDone, bInvUpdateOk, Item2Id](bool bLoad, UTestInventory* Inv, const FPlayServError&)
		{
			if (!bLoad || !Inv || Inv->Slots.Num() == 0)
			{
				*bInvUpdateDone = true;
				return;
			}

			// Swap item ref to item2 and change count
			UTestItem* NewStub = NewObject<UTestItem>(Inv);
			FPlayServDataTestAccess::BindRecordId(NewStub, *Item2Id);
			Inv->Slots[0].Item = NewStub;
			Inv->Slots[0].Count = 5;
			Inv->Slots[0].Notes = TEXT("Updated");

			PlayServ::Data::Save(Inv,FPlayServSimpleCallback::CreateLambda([bInvUpdateDone, bInvUpdateOk](bool bSave, const FPlayServError&)
			{
				*bInvUpdateOk = bSave;
				*bInvUpdateDone = true;
			}));
		});
	}, bInvUpdateDone));

	// Load inventory again and verify
	TSharedPtr<bool> bInvVerifyDone = MakeShared<bool>(false);
	TSharedPtr<bool> bInvRefUpdated = MakeShared<bool>(false);
	TSharedPtr<bool> bInvDataUpdated = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("VerifyInventory"),
		[bInvVerifyDone, bInvRefUpdated, bInvDataUpdated, InvId, Item2Id]()
	{
		PlayServ::Data::Load<UTestInventory>(*InvId,
			[bInvVerifyDone, bInvRefUpdated, bInvDataUpdated, Item2Id]
			(bool bLoad, UTestInventory* Inv, const FPlayServError&)
		{
			if (bLoad && Inv && Inv->Slots.Num() > 0)
			{
				if (Inv->Slots[0].Item)
				{
					FString LoadedItemId = UPlayServData::GetRecordId(Inv->Slots[0].Item);
					*bInvRefUpdated = LoadedItemId == *Item2Id;
				}
				*bInvDataUpdated = Inv->Slots[0].Count == 5
					&& Inv->Slots[0].Notes == TEXT("Updated");
			}
			*bInvVerifyDone = true;
		});
	}, bInvVerifyDone));

	AddCommand(new FPlayServAssertStep(this,
		[bSwapOk, bRefSwapped, bInvUpdateOk, bInvRefUpdated, bInvDataUpdated](FAutomationTestBase* T)
	{
		// Part 1: Swap clan ref
		T->TestTrue(TEXT("Save after clan ref swap succeeded"), *bSwapOk);
		T->TestTrue(TEXT("Clan ref swapped to new clan after reload"), *bRefSwapped);

		// Part 2: Update inventory slot
		T->TestTrue(TEXT("Save after inventory slot update succeeded"), *bInvUpdateOk);
		T->TestTrue(TEXT("Inventory slot item ref swapped to Item2"), *bInvRefUpdated);
		T->TestTrue(TEXT("Inventory slot data updated (Count=5, Notes=Updated)"), *bInvDataUpdated);
	}));

	AddCrudCleanupStep(this);
	return true;
}

// PlayServ.Data.Crud.UpdateNoOp
// Load, save without changes, verify no corruption.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServCrudUpdateNoOpTest,
	"PlayServ.Data.Crud.UpdateNoOp",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServCrudUpdateNoOpTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("crud-update-noop"));

	TSharedPtr<FString> EntityId = MakeShared<FString>();
	TSharedPtr<bool> bCreateDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("Create"), [bCreateDone, EntityId]()
	{
		UTestPlayer* Player = PlayServ::Data::Create<UTestPlayer>();
		Player->Name = TEXT("NoOpTarget");
		Player->Level = 15;
		Player->Score = 50.0f;

		PlayServ::Data::Save(Player, FPlayServSimpleCallback::CreateLambda([bCreateDone, EntityId, Player](bool, const FPlayServError&)
		{
			*EntityId = UPlayServData::GetRecordId(Player);
			*bCreateDone = true;
		}));
	}, bCreateDone));

	// Load then immediately save without changes
	TSharedPtr<bool> bNoOpDone = MakeShared<bool>(false);
	TSharedPtr<bool> bNoOpOk = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("SaveNoOp"), [bNoOpDone, bNoOpOk, EntityId]()
	{
		PlayServ::Data::Load<UTestPlayer>(*EntityId, [bNoOpDone, bNoOpOk](bool bLoad, UTestPlayer* Player, const FPlayServError&)
		{
			if (!bLoad || !Player)
			{
				*bNoOpDone = true;
				return;
			}
			PlayServ::Data::Save(Player,FPlayServSimpleCallback::CreateLambda([bNoOpDone, bNoOpOk](bool bSuccess, const FPlayServError&)
			{
				*bNoOpOk = bSuccess;
				*bNoOpDone = true;
			}));
		});
	}, bNoOpDone));

	// Reload and verify fields unchanged
	TSharedPtr<bool> bVerifyDone = MakeShared<bool>(false);
	TSharedPtr<bool> bFieldsMatch = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("VerifyUnchanged"), [bVerifyDone, bFieldsMatch, EntityId]()
	{
		PlayServ::Data::Load<UTestPlayer>(*EntityId, [bVerifyDone, bFieldsMatch](bool bLoad, UTestPlayer* Player, const FPlayServError&)
		{
			if (bLoad && Player)
			{
				*bFieldsMatch = Player->Name == TEXT("NoOpTarget")
					&& Player->Level == 15
					&& FMath::IsNearlyEqual(Player->Score, 50.0f, 0.01f);
			}
			*bVerifyDone = true;
		});
	}, bVerifyDone));

	AddCommand(new FPlayServAssertStep(this, [bNoOpOk, bFieldsMatch](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("No-op PSSave succeeded (no error)"), *bNoOpOk);
		T->TestTrue(TEXT("Fields unchanged after no-op save"), *bFieldsMatch);
	}));

	AddCrudCleanupStep(this);
	return true;
}

// ---------------------------------------------------------------------------
// DELETE
// ---------------------------------------------------------------------------

// PlayServ.Data.Crud.DeleteStatic
// Create, verify exists, PSDelete by ID, verify 404.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServCrudDeleteStaticTest,
	"PlayServ.Data.Crud.DeleteStatic",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServCrudDeleteStaticTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("crud-delete-static"));

	TSharedPtr<FString> EntityId = MakeShared<FString>();
	TSharedPtr<bool> bCreateDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("Create"), [bCreateDone, EntityId]()
	{
		UTestPlayer* Player = PlayServ::Data::Create<UTestPlayer>();
		Player->Name = TEXT("DeleteTarget");
		Player->Level = 1;

		PlayServ::Data::Save(Player, FPlayServSimpleCallback::CreateLambda([bCreateDone, EntityId, Player](bool, const FPlayServError&)
		{
			*EntityId = UPlayServData::GetRecordId(Player);
			*bCreateDone = true;
		}));
	}, bCreateDone));

	// Verify exists before delete
	TSharedPtr<bool> bExistsDone = MakeShared<bool>(false);
	TSharedPtr<bool> bExists = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("VerifyExists"), [bExistsDone, bExists, EntityId]()
	{
		PlayServ::Data::Load<UTestPlayer>(*EntityId, [bExistsDone, bExists](bool bSuccess, UTestPlayer*, const FPlayServError&)
		{
			*bExists = bSuccess;
			*bExistsDone = true;
		});
	}, bExistsDone));

	// Delete by ID
	TSharedPtr<bool> bDeleteDone = MakeShared<bool>(false);
	TSharedPtr<bool> bDeleteOk = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("Delete"), [bDeleteDone, bDeleteOk, EntityId]()
	{
		PlayServ::Data::DeleteById<UTestPlayer>(*EntityId, FPlayServSimpleCallback::CreateLambda(
			[bDeleteDone, bDeleteOk](bool bSuccess, const FPlayServError&)
			{
				*bDeleteOk = bSuccess;
				*bDeleteDone = true;
			}));
	}, bDeleteDone));

	// Verify 404
	TSharedPtr<bool> bVerifyDone = MakeShared<bool>(false);
	TSharedPtr<bool> bVerify404 = MakeShared<bool>(false);
	TSharedPtr<EPlayServErrorCode> Verify404Code = MakeShared<EPlayServErrorCode>(EPlayServErrorCode::None);
	AddCommand(new FPlayServAsyncStep(this, TEXT("Verify404"), [bVerifyDone, bVerify404, Verify404Code, EntityId]()
	{
		PlayServ::Data::Load<UTestPlayer>(*EntityId, [bVerifyDone, bVerify404, Verify404Code](bool bSuccess, UTestPlayer*, const FPlayServError& Error)
		{
			*bVerify404 = !bSuccess;
			*Verify404Code = Error.Code;
			*bVerifyDone = true;
		});
	}, bVerifyDone));

	AddCommand(new FPlayServAssertStep(this, [bExists, bDeleteOk, bVerify404, Verify404Code, EntityId](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("Entity existed before delete"), *bExists);
		T->TestTrue(TEXT("PSDelete succeeded"), *bDeleteOk);
		T->TestTrue(TEXT("PSLoad after delete returns 404"), *bVerify404);
		T->TestEqual(TEXT("Load after delete maps 404 to NotFound"), *Verify404Code, EPlayServErrorCode::NotFound);
		T->AddInfo(FString::Printf(TEXT("Entity %s: exists -> PSDelete -> 404"), **EntityId));
	}));

	AddCrudCleanupStep(this);
	return true;
}

// PlayServ.Data.Crud.DeleteSelf
// Create, save, load, PSDeleteSelf on loaded instance, verify 404.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServCrudDeleteSelfTest,
	"PlayServ.Data.Crud.DeleteSelf",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServCrudDeleteSelfTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("crud-delete-self"));

	TSharedPtr<FString> EntityId = MakeShared<FString>();
	TSharedPtr<bool> bCreateDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("Create"), [bCreateDone, EntityId]()
	{
		UTestPlayer* Player = PlayServ::Data::Create<UTestPlayer>();
		Player->Name = TEXT("SelfDeleteTarget");
		Player->Level = 7;

		PlayServ::Data::Save(Player, FPlayServSimpleCallback::CreateLambda([bCreateDone, EntityId, Player](bool, const FPlayServError&)
		{
			*EntityId = UPlayServData::GetRecordId(Player);
			*bCreateDone = true;
		}));
	}, bCreateDone));

	// Load the entity, then call PSDeleteSelf on the loaded instance
	TSharedPtr<bool> bDeleteSelfDone = MakeShared<bool>(false);
	TSharedPtr<bool> bDeleteSelfOk = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("DeleteSelf"), [bDeleteSelfDone, bDeleteSelfOk, EntityId]()
	{
		PlayServ::Data::Load<UTestPlayer>(*EntityId, [bDeleteSelfDone, bDeleteSelfOk](bool bLoad, UTestPlayer* Player, const FPlayServError&)
		{
			if (!bLoad || !Player)
			{
				*bDeleteSelfDone = true;
				return;
			}
			PlayServ::Data::Delete(Player, FPlayServSimpleCallback::CreateLambda(
				[bDeleteSelfDone, bDeleteSelfOk](bool bSuccess, const FPlayServError&)
				{
					*bDeleteSelfOk = bSuccess;
					*bDeleteSelfDone = true;
				}));
		});
	}, bDeleteSelfDone));

	// Verify 404
	TSharedPtr<bool> bVerifyDone = MakeShared<bool>(false);
	TSharedPtr<bool> bVerify404 = MakeShared<bool>(false);
	TSharedPtr<EPlayServErrorCode> Verify404Code = MakeShared<EPlayServErrorCode>(EPlayServErrorCode::None);
	AddCommand(new FPlayServAsyncStep(this, TEXT("Verify404"), [bVerifyDone, bVerify404, Verify404Code, EntityId]()
	{
		PlayServ::Data::Load<UTestPlayer>(*EntityId, [bVerifyDone, bVerify404, Verify404Code](bool bSuccess, UTestPlayer*, const FPlayServError& Error)
		{
			*bVerify404 = !bSuccess;
			*Verify404Code = Error.Code;
			*bVerifyDone = true;
		});
	}, bVerifyDone));

	AddCommand(new FPlayServAssertStep(this, [bDeleteSelfOk, bVerify404, Verify404Code, EntityId](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("PSDeleteSelf succeeded"), *bDeleteSelfOk);
		T->TestTrue(TEXT("PSLoad after PSDeleteSelf returns 404"), *bVerify404);
		T->TestEqual(TEXT("Load after PSDeleteSelf maps 404 to NotFound"), *Verify404Code, EPlayServErrorCode::NotFound);
		T->AddInfo(FString::Printf(TEXT("Entity %s: create -> load -> PSDeleteSelf -> 404"), **EntityId));
	}));

	AddCrudCleanupStep(this);
	return true;
}

// Batch Create

// PlayServ.Data.Crud.BatchCreate
// SaveAllEntities creates 3 entities in a single batch, verify all saved.

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServCrudBatchCreateTest,
	"PlayServ.Data.Crud.BatchCreate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServCrudBatchCreateTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("crud-batch-create"));

	TSharedPtr<bool> bBatchDone = MakeShared<bool>(false);
	TSharedPtr<bool> bBatchOk = MakeShared<bool>(false);
	TSharedPtr<TArray<FString>> CreatedIds = MakeShared<TArray<FString>>();
	AddCommand(new FPlayServAsyncStep(this, TEXT("BatchCreate"), [bBatchDone, bBatchOk, CreatedIds]()
	{
		UPlayServSubsystem* PS = UPlayServSubsystem::Get();
		TArray<UObject*> Batch;
		for (int32 i = 0; i < 3; i++)
		{
			UTestPlayer* P = PlayServ::Data::Create<UTestPlayer>();
			P->Name = FString::Printf(TEXT("BatchPlayer_%d"), i);
			P->Level = (i + 1) * 10;
			Batch.Add(P);
		}

		PlayServ::Data::BulkSave(Batch, FPlayServSimpleCallback::CreateLambda(
			[bBatchDone, bBatchOk, CreatedIds, Batch](bool bSuccess, const FPlayServError&)
			{
				*bBatchOk = bSuccess;
				for (UObject* Obj : Batch)
				{
					CreatedIds->Add(UPlayServData::GetRecordId(Obj));
				}
				*bBatchDone = true;
			}));
	}, bBatchDone));

	// Verify all 3 can be loaded back individually
	TSharedPtr<bool> bVerifyDone = MakeShared<bool>(false);
	TSharedPtr<int32> LoadedCount = MakeShared<int32>(0);
	TSharedPtr<int32> Remaining = MakeShared<int32>(3);
	AddCommand(new FPlayServAsyncStep(this, TEXT("VerifyLoads"), [bVerifyDone, LoadedCount, Remaining, CreatedIds]()
	{
		for (const FString& Id : *CreatedIds)
		{
			PlayServ::Data::Load<UTestPlayer>(Id, [bVerifyDone, LoadedCount, Remaining](bool bSuccess, UTestPlayer* Player, const FPlayServError&)
			{
				if (bSuccess && Player)
				{
					++(*LoadedCount);
				}
				if (--(*Remaining) == 0)
				{
					*bVerifyDone = true;
				}
			});
		}
	}, bVerifyDone, 3.0f));

	AddCommand(new FPlayServAssertStep(this, [bBatchOk, LoadedCount](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("SaveAllEntities batch succeeded"), *bBatchOk);
		T->TestEqual(TEXT("All 3 entities loadable after batch create"), *LoadedCount, 3);
	}));

	AddCrudCleanupStep(this);
	return true;
}

// ---------------------------------------------------------------------------
// NOT FOUND + LOAD-OR-CREATE
// ---------------------------------------------------------------------------

// PlayServ.Data.Crud.LoadMissingReturnsNotFound
// Load a well-formed rec_* id the server never minted; the failure must classify as
// EPlayServErrorCode::NotFound (not the generic Unknown), with the platform machine code
// `not_found` carried in ProblemCode — the structured signal that replaces "HTTP 404" matching.
// (The old UnauthenticatedMapsUnauthorized sibling died with V2: the fixture tables are unowned
// client-writable, so a logged-out pk_-only caller can read them by design — the logged-out
// denial test now lives in PlayServ.Data.ErrorHandling.LoggedOutClosedTableForbidden.)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServCrudLoadMissingReturnsNotFoundTest,
	"PlayServ.Data.Crud.LoadMissingReturnsNotFound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServCrudLoadMissingReturnsNotFoundTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("crud-load-missing"));

	const FString MissingId = CrudMissingRecId;

	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	TSharedPtr<bool> bSuccess = MakeShared<bool>(true);
	TSharedPtr<EPlayServErrorCode> Code = MakeShared<EPlayServErrorCode>(EPlayServErrorCode::None);
	TSharedPtr<FString> ProblemCode = MakeShared<FString>();
	AddCommand(new FPlayServAsyncStep(this, TEXT("LoadMissing"), [bDone, bSuccess, Code, ProblemCode, MissingId]()
	{
		PlayServ::Data::Load<UTestPlayer>(MissingId, [bDone, bSuccess, Code, ProblemCode](bool bOk, UTestPlayer*, const FPlayServError& Error)
		{
			*bSuccess = bOk;
			*Code = Error.Code;
			*ProblemCode = Error.ProblemCode;
			*bDone = true;
		});
	}, bDone));

	AddCommand(new FPlayServAssertStep(this, [bSuccess, Code, ProblemCode, MissingId](FAutomationTestBase* T)
	{
		T->TestFalse(TEXT("Load of never-created entity fails"), *bSuccess);
		T->TestEqual(TEXT("Missing entity maps to NotFound (not Unknown)"), *Code, EPlayServErrorCode::NotFound);
		T->TestEqual(TEXT("ProblemCode carries the platform machine code"), *ProblemCode, FString(TEXT("not_found")));
		T->AddInfo(FString::Printf(TEXT("Loaded missing id %s -> NotFound/%s"), *MissingId, **ProblemCode));
	}));

	return true;
}

#endif // !UE_BUILD_SHIPPING
