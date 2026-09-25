#include "PlayServTestCommon.h"
#include "PlayServTestAccess.h"
#include "Core/PlayServSubsystem.h"
#include "Auth/PlayServAuth.h"
#include "Data/PlayServData.h"
#include "PlayServ.h"
#include "Data/PlayServFilter.h"
#include "TestEntities.h"

#if !UE_BUILD_SHIPPING

// ---------------------------------------------------------------------------
// PlayServ.Data.Collection.* — semantics under test:
//
// - Record ids are SERVER-minted: seeding captures ids from save callbacks where rows must be
//   addressable; a stub for a known id is bound through FPlayServDataTestAccess.
// - AddLoginStep is an ANONYMOUS login (fresh plr_* per run) and the fixture tables are UNOWNED
//   shared rows, so leftovers from a crashed prior run can coexist with this run's seed data.
//   Filter tests therefore tag every seeded Name with a per-run Marker prefix and assert on the
//   marker-scoped subset of results (exact counts stay deterministic without wiping the table).
//   Dedicated count/wipe tests (DeleteAllWithVerify) keep the wipe-based shape instead.
// - Dot-notation filter paths into inclusion parts are addressable — see FilterNestedPathAccepted /
//   FilterUndeclaredNestedPathRejected.
// ---------------------------------------------------------------------------

// Cleanup helper — delete all collection test entity types then logout. 15s: DeleteAll is
// query-then-fan-out on V2, so a leaked table from a crashed prior run can take a while.
static void AddCollectionCleanupStep(FAutomationTestBase* Test)
{
	// Two waves: referencing types (TestPlayer holds Clan refs; TestInventory points at
	// TestItem) before referenced types — V2 relation links 409 in_use on a concurrent wipe.
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
	}, bDone, 20.0f));
}

// Helper: create N UTestPlayer entities with a setup lambda, batch save
static void AddCreatePlayersStep(
	FAutomationTestBase* Test,
	const FString& StepName,
	int32 Count,
	TFunction<void(UTestPlayer* Player, int32 Index)> SetupFn)
{
	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	Test->AddCommand(new FPlayServAsyncStep(Test, StepName, [bDone, Count, SetupFn = MoveTemp(SetupFn)]()
	{
		UPlayServSubsystem* PS = UPlayServSubsystem::Get();
		if (!PS) { *bDone = true; return; }
		TArray<UObject*> Batch;
		for (int32 i = 0; i < Count; ++i)
		{
			UTestPlayer* P = PlayServ::Data::Create<UTestPlayer>();
			SetupFn(P, i);
			Batch.Add(P);
		}
		PlayServ::Data::BulkSave(Batch, FPlayServSimpleCallback::CreateLambda(
			[bDone](bool, const FPlayServError&) { *bDone = true; }));
	}, bDone, 3.0f));
}

// Helper: create a clan + 6 players (3 with clan ref, 3 without) for IsNull/IsNotNull tests.
// Names arrive pre-marked by the caller; the clan's server id is captured from its save callback.
static void AddCreateClanAndPlayersStep(
	FAutomationTestBase* Test,
	const FString& ClanName,
	const TArray<FString>& WithClanNames,
	const TArray<FString>& NoClanNames)
{
	TSharedPtr<FString> ClanId = MakeShared<FString>();
	TSharedPtr<bool> bClanDone = MakeShared<bool>(false);
	Test->AddCommand(new FPlayServAsyncStep(Test, TEXT("CreateClan"), [bClanDone, ClanId, ClanName]()
	{
		UTestClan* Clan = PlayServ::Data::Create<UTestClan>();
		Clan->ClanName = ClanName;
		PlayServ::Data::Save(Clan, FPlayServSimpleCallback::CreateLambda([bClanDone, ClanId, Clan](bool, const FPlayServError&)
		{
			*ClanId = UPlayServData::GetRecordId(Clan);
			*bClanDone = true;
		}));
	}, bClanDone));

	TSharedPtr<bool> bPlayersDone = MakeShared<bool>(false);
	Test->AddCommand(new FPlayServAsyncStep(Test, TEXT("CreatePlayers"),
		[bPlayersDone, ClanId, WithNames = WithClanNames, NoNames = NoClanNames]()
	{
		UPlayServSubsystem* PS = UPlayServSubsystem::Get();
		if (!PS) { *bPlayersDone = true; return; }
		TArray<UObject*> Batch;

		for (const FString& Name : WithNames)
		{
			UTestPlayer* P = PlayServ::Data::Create<UTestPlayer>();
			P->Name = Name;
			P->Level = 1;
			UTestClan* Stub = NewObject<UTestClan>(PS);
			FPlayServDataTestAccess::BindRecordId(Stub, *ClanId);
			P->Clan = Stub;
			Batch.Add(P);
		}

		for (const FString& Name : NoNames)
		{
			UTestPlayer* P = PlayServ::Data::Create<UTestPlayer>();
			P->Name = Name;
			P->Level = 1;
			Batch.Add(P);
		}

		PlayServ::Data::BulkSave(Batch, FPlayServSimpleCallback::CreateLambda(
			[bPlayersDone](bool, const FPlayServError&) { *bPlayersDone = true; }));
	}, bPlayersDone));
}

struct FFilterResult
{
	int32 Count = 0;
	TArray<FString> Names;

	bool ContainsName(const FString& Name) const
	{
		return Names.Contains(Name);
	}

	// Rows seeded by THIS run — queries on shared numeric/bool fields also return leftovers
	// from crashed prior runs, so exact-count assertions scope to the per-run marker prefix.
	int32 CountWithPrefix(const FString& Prefix) const
	{
		int32 Matching = 0;
		for (const FString& Name : Names)
		{
			if (Name.StartsWith(Prefix))
			{
				++Matching;
			}
		}
		return Matching;
	}

	FString NamesAsString() const
	{
		return FString::Join(Names, TEXT(", "));
	}

	void LogTo(FAutomationTestBase* T) const
	{
		T->AddInfo(FString::Printf(TEXT("Returned: [%s]"), *NamesAsString()));
	}
};

// Helper: run a filter query and capture count + names
static void AddFilterQueryStep(
	FAutomationTestBase* Test,
	const FString& StepName,
	FPlayServFilter Filter,
	TSharedPtr<FFilterResult> OutResult)
{
	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	Test->AddCommand(new FPlayServAsyncStep(Test, StepName,
		[bDone, OutResult, Filter = MoveTemp(Filter)]()
	{
		PlayServ::Data::LoadAll<UTestPlayer>(Filter,
			[bDone, OutResult](bool bSuccess, TArray<UTestPlayer*> Results, const FPlayServError&)
		{
			if (bSuccess)
			{
				OutResult->Count = Results.Num();
				for (UTestPlayer* P : Results)
				{
					if (P)
					{
						OutResult->Names.Add(P->Name);
					}
				}
			}
			*bDone = true;
		});
	}, bDone, 3.0f));
}

// ============================================================================
// BASIC COLLECTION OPERATIONS
// ============================================================================

// Create 6 items, delete 4 via DeleteEntities(Array), verify 2 remain with correct names.
// Item ids are captured from the batch-create callback; the deletes bind them to stubs.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServCollectionDeleteEntitiesTest,
	"PlayServ.Data.Collection.DeleteEntities",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServCollectionDeleteEntitiesTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("coll-delete-entities"));

	const FString Marker = FGuid::NewGuid().ToString();

	TSharedPtr<TArray<FString>> ItemIds = MakeShared<TArray<FString>>();
	TSharedPtr<bool> bCreateDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("CreateItems"), [bCreateDone, ItemIds, Marker]()
	{
		UPlayServSubsystem* PS = UPlayServSubsystem::Get();
		if (!PS) { *bCreateDone = true; return; }
		TArray<UObject*> Batch;
		const TCHAR* Names[] = { TEXT("Sword"), TEXT("Shield"), TEXT("Bow"), TEXT("Staff"), TEXT("Dagger"), TEXT("Helm") };
		for (int32 i = 0; i < 6; ++i)
		{
			UTestItem* Item = PlayServ::Data::Create<UTestItem>();
			Item->ItemName = Marker + TEXT("-") + Names[i];
			Item->Power = (i + 1) * 10;
			Batch.Add(Item);
		}
		// Server ids exist only after the batch create lands — capture in creation order.
		PlayServ::Data::BulkSave(Batch, FPlayServSimpleCallback::CreateLambda(
			[bCreateDone, ItemIds, Batch](bool, const FPlayServError&)
		{
			for (UObject* Obj : Batch)
			{
				ItemIds->Add(UPlayServData::GetRecordId(Obj));
			}
			*bCreateDone = true;
		}));
	}, bCreateDone));

	TSharedPtr<bool> bDeleteDone = MakeShared<bool>(false);
	TSharedPtr<bool> bDeleteOk = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("DeleteFour"), [bDeleteDone, bDeleteOk, ItemIds]()
	{
		UPlayServSubsystem* PS = UPlayServSubsystem::Get();
		if (!PS) { *bDeleteDone = true; return; }
		TArray<UObject*> ToDelete;
		for (int32 i = 0; i < 4; ++i)
		{
			UTestItem* Stub = NewObject<UTestItem>(PS);
			FPlayServDataTestAccess::BindRecordId(Stub, (*ItemIds)[i]);
			ToDelete.Add(Stub);
		}
		PlayServ::Data::BulkDelete(ToDelete, FPlayServSimpleCallback::CreateLambda(
			[bDeleteDone, bDeleteOk](bool bSuccess, const FPlayServError&)
			{
				*bDeleteOk = bSuccess;
				*bDeleteDone = true;
			}));
	}, bDeleteDone, 3.0f));

	TSharedPtr<bool> bVerifyDone = MakeShared<bool>(false);
	TSharedPtr<TArray<FString>> SurvivorNames = MakeShared<TArray<FString>>();
	AddCommand(new FPlayServAsyncStep(this, TEXT("VerifyRemaining"), [bVerifyDone, SurvivorNames]()
	{
		PlayServ::Data::LoadAll<UTestItem>(FPlayServFilter::None(),
			[bVerifyDone, SurvivorNames](bool bSuccess, TArray<UTestItem*> Results, const FPlayServError&)
		{
			if (bSuccess)
			{
				for (UTestItem* Item : Results)
				{
					if (Item)
					{
						SurvivorNames->Add(Item->ItemName);
					}
				}
			}
			*bVerifyDone = true;
		});
	}, bVerifyDone));

	AddCommand(new FPlayServAssertStep(this, [bDeleteOk, SurvivorNames, Marker](FAutomationTestBase* T)
	{
		int32 MarkedSurvivors = 0;
		for (const FString& Name : *SurvivorNames)
		{
			if (Name.StartsWith(Marker)) { ++MarkedSurvivors; }
		}
		T->TestTrue(TEXT("DeleteEntities succeeded"), *bDeleteOk);
		T->TestEqual(TEXT("2 of this run's items remain after deleting 4 of 6"), MarkedSurvivors, 2);
		T->TestTrue(TEXT("Dagger survived"), SurvivorNames->Contains(Marker + TEXT("-Dagger")));
		T->TestTrue(TEXT("Helm survived"), SurvivorNames->Contains(Marker + TEXT("-Helm")));
		T->TestFalse(TEXT("Sword deleted"), SurvivorNames->Contains(Marker + TEXT("-Sword")));
		T->TestFalse(TEXT("Shield deleted"), SurvivorNames->Contains(Marker + TEXT("-Shield")));
		T->TestFalse(TEXT("Bow deleted"), SurvivorNames->Contains(Marker + TEXT("-Bow")));
		T->TestFalse(TEXT("Staff deleted"), SurvivorNames->Contains(Marker + TEXT("-Staff")));
		T->AddInfo(FString::Printf(TEXT("Survivors: [%s]"), *FString::Join(*SurvivorNames, TEXT(", "))));
	}));

	AddCollectionCleanupStep(this);
	return true;
}

// PSDeleteAll with None() — SDK rejects without backend call.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServCollectionDeleteAllSafetyGuardTest,
	"PlayServ.Data.Collection.DeleteAllSafetyGuard",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServCollectionDeleteAllSafetyGuardTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("coll-safety-guard"));

	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	TSharedPtr<bool> bRejected = MakeShared<bool>(false);
	TSharedPtr<FString> ErrorMsg = MakeShared<FString>();
	AddCommand(new FPlayServAsyncStep(this, TEXT("DeleteAllNone"), [bDone, bRejected, ErrorMsg]()
	{
		PlayServ::Data::DeleteAll<UTestPlayer>(FPlayServFilter::None(), FPlayServDeleteAllCallback::CreateLambda(
			[bDone, bRejected, ErrorMsg](bool bSuccess, const FPlayServDeleteAllResult&, const FPlayServError& Error)
			{
				*bRejected = !bSuccess;
				*ErrorMsg = Error.Message;
				*bDone = true;
			}));
	}, bDone));

	AddCommand(new FPlayServAssertStep(this, [bRejected, ErrorMsg](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("PSDeleteAll(None()) rejected by SDK"), *bRejected);
		T->TestTrue(TEXT("Error message mentions All()"), (*ErrorMsg).Contains(TEXT("All()")));
		T->AddInfo(FString::Printf(TEXT("Error: %s"), **ErrorMsg));
	}));

	AddCollectionCleanupStep(this);
	return true;
}

// PSDeleteAll with default-constructed FPlayServFilter() — must also be rejected.
// A default filter has no conditions and is not explicitly All(), so it should be
// caught by the same safety guard as None().
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServCollectionDeleteAllDefaultFilterTest,
	"PlayServ.Data.Collection.DeleteAllRejectsDefaultFilter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServCollectionDeleteAllDefaultFilterTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("coll-default-filter"));

	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	TSharedPtr<bool> bRejected = MakeShared<bool>(false);
	TSharedPtr<FString> ErrorMsg = MakeShared<FString>();
	AddCommand(new FPlayServAsyncStep(this, TEXT("DeleteAllDefault"), [bDone, bRejected, ErrorMsg]()
	{
		PlayServ::Data::DeleteAll<UTestPlayer>(FPlayServFilter(), FPlayServDeleteAllCallback::CreateLambda(
			[bDone, bRejected, ErrorMsg](bool bSuccess, const FPlayServDeleteAllResult&, const FPlayServError& Error)
			{
				*bRejected = !bSuccess;
				*ErrorMsg = Error.Message;
				*bDone = true;
			}));
	}, bDone));

	AddCommand(new FPlayServAssertStep(this, [bRejected, ErrorMsg](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("PSDeleteAll(FPlayServFilter()) rejected by SDK"), *bRejected);
		T->TestTrue(TEXT("Error message mentions All()"), (*ErrorMsg).Contains(TEXT("All()")));
		T->AddInfo(FString::Printf(TEXT("Error: %s"), **ErrorMsg));
	}));

	AddCollectionCleanupStep(this);
	return true;
}

// Filter on a non-existent field — the platform rejects it deterministically as 422
// ValidationFailed.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServCollectionMalformedUnknownFieldTest,
	"PlayServ.Data.Collection.MalformedFilterUnknownField",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServCollectionMalformedUnknownFieldTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("coll-malformed-field"));

	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	TSharedPtr<bool> bQueryOk = MakeShared<bool>(true);
	TSharedPtr<EPlayServErrorCode> Code = MakeShared<EPlayServErrorCode>(EPlayServErrorCode::None);
	TSharedPtr<FString> ProblemCode = MakeShared<FString>();
	TSharedPtr<FString> ErrorMsg = MakeShared<FString>();
	AddCommand(new FPlayServAsyncStep(this, TEXT("QueryUnknownField"), [bDone, bQueryOk, Code, ProblemCode, ErrorMsg]()
	{
		FPlayServFilter Filter = FPlayServFilter::Where(TEXT("NonExistentField")).EqualTo(FString(TEXT("x")));
		PlayServ::Data::LoadAll<UTestPlayer>(Filter,
			[bDone, bQueryOk, Code, ProblemCode, ErrorMsg](bool bSuccess, TArray<UTestPlayer*> Results, const FPlayServError& Error)
		{
			*bQueryOk = bSuccess;
			*Code = Error.Code;
			*ProblemCode = Error.ProblemCode;
			*ErrorMsg = Error.Message;
			*bDone = true;
		});
	}, bDone, 3.0f));

	AddCommand(new FPlayServAssertStep(this, [bQueryOk, Code, ProblemCode, ErrorMsg](FAutomationTestBase* T)
	{
		T->TestFalse(TEXT("Filter on unknown field fails"), *bQueryOk);
		T->TestEqual(TEXT("422 maps to ValidationFailed"), *Code, EPlayServErrorCode::ValidationFailed);
		T->TestFalse(TEXT("ProblemCode carries the platform machine code"), (*ProblemCode).IsEmpty());
		T->AddInfo(FString::Printf(TEXT("ProblemCode=%s, Error=%s"), **ProblemCode, **ErrorMsg));
	}));

	AddCollectionCleanupStep(this);
	return true;
}

// Observational: filter with wrong value type for a field. V2 may 422 it or match nothing —
// not pinned; the assertion is only that the SDK does not crash and the callback fires.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServCollectionMalformedTypeMismatchTest,
	"PlayServ.Data.Collection.MalformedFilterTypeMismatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServCollectionMalformedTypeMismatchTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("coll-malformed-type"));

	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	TSharedPtr<bool> bQueryOk = MakeShared<bool>(false);
	TSharedPtr<int32> ResultCount = MakeShared<int32>(-1);
	TSharedPtr<FString> ErrorMsg = MakeShared<FString>();
	AddCommand(new FPlayServAsyncStep(this, TEXT("QueryTypeMismatch"), [bDone, bQueryOk, ResultCount, ErrorMsg]()
	{
		FPlayServFilter Filter = FPlayServFilter::Where(TEXT("Level")).EqualTo(FString(TEXT("not_a_number")));
		PlayServ::Data::LoadAll<UTestPlayer>(Filter,
			[bDone, bQueryOk, ResultCount, ErrorMsg](bool bSuccess, TArray<UTestPlayer*> Results, const FPlayServError& Error)
		{
			*bQueryOk = bSuccess;
			*ResultCount = Results.Num();
			*ErrorMsg = Error.Message;
			*bDone = true;
		});
	}, bDone, 3.0f));

	AddCommand(new FPlayServAssertStep(this, [bQueryOk, ResultCount, ErrorMsg](FAutomationTestBase* T)
	{
		T->AddInfo(FString::Printf(TEXT("Success=%s, Count=%d, Error=%s"),
			*bQueryOk ? TEXT("true") : TEXT("false"), *ResultCount, **ErrorMsg));
	}));

	AddCollectionCleanupStep(this);
	return true;
}

// Observational: two operators on same field without .And(). Contradictory AND conditions are
// grammatically VALID on V2 (expected: success with zero rows) — recorded, not pinned.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServCollectionMalformedConflictingOpsTest,
	"PlayServ.Data.Collection.MalformedFilterConflictingOps",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServCollectionMalformedConflictingOpsTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("coll-malformed-conflict"));

	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	TSharedPtr<bool> bQueryOk = MakeShared<bool>(false);
	TSharedPtr<int32> ResultCount = MakeShared<int32>(-1);
	TSharedPtr<FString> ErrorMsg = MakeShared<FString>();
	AddCommand(new FPlayServAsyncStep(this, TEXT("QueryConflicting"), [bDone, bQueryOk, ResultCount, ErrorMsg]()
	{
		TArray<FString> InValues = { TEXT("20"), TEXT("30") };
		FPlayServFilter Filter = FPlayServFilter::Where(TEXT("Level")).EqualTo(10).And(TEXT("Level")).In(InValues);
		PlayServ::Data::LoadAll<UTestPlayer>(Filter,
			[bDone, bQueryOk, ResultCount, ErrorMsg](bool bSuccess, TArray<UTestPlayer*> Results, const FPlayServError& Error)
		{
			*bQueryOk = bSuccess;
			*ResultCount = Results.Num();
			*ErrorMsg = Error.Message;
			*bDone = true;
		});
	}, bDone, 3.0f));

	AddCommand(new FPlayServAssertStep(this, [bQueryOk, ResultCount, ErrorMsg](FAutomationTestBase* T)
	{
		T->AddInfo(FString::Printf(TEXT("Success=%s, Count=%d, Error=%s"),
			*bQueryOk ? TEXT("true") : TEXT("false"), *ResultCount, **ErrorMsg));
	}));

	AddCollectionCleanupStep(this);
	return true;
}

// ============================================================================
// FILTER TESTS — each with 6+ entities for thorough verification.
// Seeded Names carry the per-run Marker prefix; exact counts assert the marker-scoped subset
// (queries on shared numeric/bool fields also return leftovers from crashed prior runs).
// ============================================================================

// 6 players, 2 named "<Marker>-Bob" — verify exactly those 2 returned (exact-match query is
// marker-scoped by its value, so the raw result count is deterministic here)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServFilterEqualToStringTest,
	"PlayServ.Data.Collection.FilterEqualToString",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServFilterEqualToStringTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("filter-eq-str"));

	const FString Marker = FGuid::NewGuid().ToString();

	AddCreatePlayersStep(this, TEXT("Create"), 6, [Marker](UTestPlayer* P, int32 i)
	{
		const TCHAR* Names[] = { TEXT("Alice"), TEXT("Bob"), TEXT("Charlie"), TEXT("Bob"), TEXT("Diana"), TEXT("Elena") };
		P->Name = Marker + TEXT("-") + Names[i];
		P->Level = (i + 1) * 10;
	});

	TSharedPtr<FFilterResult> Result = MakeShared<FFilterResult>();
	AddFilterQueryStep(this, TEXT("Query"),
		FPlayServFilter::Where(TEXT("Name")).EqualTo(Marker + TEXT("-Bob")), Result);

	AddCommand(new FPlayServAssertStep(this, [Result, Marker](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("EqualTo(\"Bob\") count"), Result->Count, 2);
		T->TestFalse(TEXT("Excludes Alice"), Result->ContainsName(Marker + TEXT("-Alice")));
		T->TestFalse(TEXT("Excludes Charlie"), Result->ContainsName(Marker + TEXT("-Charlie")));
		T->TestFalse(TEXT("Excludes Diana"), Result->ContainsName(Marker + TEXT("-Diana")));
		T->TestFalse(TEXT("Excludes Elena"), Result->ContainsName(Marker + TEXT("-Elena")));
		int32 BobCount = 0;
		for (const FString& N : Result->Names) { if (N == Marker + TEXT("-Bob")) { ++BobCount; } }
		T->TestEqual(TEXT("All returned are Bob"), BobCount, 2);
		Result->LogTo(T);
	}));

	AddCollectionCleanupStep(this);
	return true;
}

// 6 players with levels 5,42,42,42,80,100 — EqualTo(42) expects 3 of this run's rows
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServFilterEqualToIntTest,
	"PlayServ.Data.Collection.FilterEqualToInt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServFilterEqualToIntTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("filter-eq-int"));

	const FString Marker = FGuid::NewGuid().ToString();

	AddCreatePlayersStep(this, TEXT("Create"), 6, [Marker](UTestPlayer* P, int32 i)
	{
		const TCHAR* Names[] = { TEXT("Lo"), TEXT("Match1"), TEXT("Match2"), TEXT("Match3"), TEXT("Hi"), TEXT("VeryHi") };
		int32 Levels[] = { 5, 42, 42, 42, 80, 100 };
		P->Name = Marker + TEXT("-") + Names[i];
		P->Level = Levels[i];
	});

	TSharedPtr<FFilterResult> Result = MakeShared<FFilterResult>();
	AddFilterQueryStep(this, TEXT("Query"),
		FPlayServFilter::Where(TEXT("Level")).EqualTo(42), Result);

	AddCommand(new FPlayServAssertStep(this, [Result, Marker](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("EqualTo(42) count (this run's rows)"), Result->CountWithPrefix(Marker), 3);
		T->TestTrue(TEXT("Contains Match1"), Result->ContainsName(Marker + TEXT("-Match1")));
		T->TestTrue(TEXT("Contains Match2"), Result->ContainsName(Marker + TEXT("-Match2")));
		T->TestTrue(TEXT("Contains Match3"), Result->ContainsName(Marker + TEXT("-Match3")));
		T->TestFalse(TEXT("Excludes Lo (5)"), Result->ContainsName(Marker + TEXT("-Lo")));
		T->TestFalse(TEXT("Excludes Hi (80)"), Result->ContainsName(Marker + TEXT("-Hi")));
		T->TestFalse(TEXT("Excludes VeryHi (100)"), Result->ContainsName(Marker + TEXT("-VeryHi")));
		Result->LogTo(T);
	}));

	AddCollectionCleanupStep(this);
	return true;
}

// 6 players: 4 active, 2 inactive — EqualTo(true) expects 4 of this run's rows
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServFilterEqualToBoolTest,
	"PlayServ.Data.Collection.FilterEqualToBool",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServFilterEqualToBoolTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("filter-eq-bool"));

	const FString Marker = FGuid::NewGuid().ToString();

	AddCreatePlayersStep(this, TEXT("Create"), 6, [Marker](UTestPlayer* P, int32 i)
	{
		const TCHAR* Names[] = { TEXT("Active1"), TEXT("Inactive1"), TEXT("Active2"), TEXT("Active3"), TEXT("Inactive2"), TEXT("Active4") };
		bool Active[] = { true, false, true, true, false, true };
		P->Name = Marker + TEXT("-") + Names[i];
		P->Level = 1;
		P->bActive = Active[i];
	});

	TSharedPtr<FFilterResult> Result = MakeShared<FFilterResult>();
	AddFilterQueryStep(this, TEXT("Query"),
		FPlayServFilter::Where(TEXT("bActive")).EqualTo(true), Result);

	AddCommand(new FPlayServAssertStep(this, [Result, Marker](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("EqualTo(true) count (this run's rows)"), Result->CountWithPrefix(Marker), 4);
		T->TestTrue(TEXT("Contains Active1"), Result->ContainsName(Marker + TEXT("-Active1")));
		T->TestTrue(TEXT("Contains Active2"), Result->ContainsName(Marker + TEXT("-Active2")));
		T->TestTrue(TEXT("Contains Active3"), Result->ContainsName(Marker + TEXT("-Active3")));
		T->TestTrue(TEXT("Contains Active4"), Result->ContainsName(Marker + TEXT("-Active4")));
		T->TestFalse(TEXT("Excludes Inactive1"), Result->ContainsName(Marker + TEXT("-Inactive1")));
		T->TestFalse(TEXT("Excludes Inactive2"), Result->ContainsName(Marker + TEXT("-Inactive2")));
		Result->LogTo(T);
	}));

	AddCollectionCleanupStep(this);
	return true;
}

// 6 players — NotEqualTo("<Marker>-Charlie") expects 5 of this run's rows
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServFilterNotEqualToStringTest,
	"PlayServ.Data.Collection.FilterNotEqualToString",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServFilterNotEqualToStringTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("filter-ne-str"));

	const FString Marker = FGuid::NewGuid().ToString();

	AddCreatePlayersStep(this, TEXT("Create"), 6, [Marker](UTestPlayer* P, int32 i)
	{
		const TCHAR* Names[] = { TEXT("Alice"), TEXT("Bob"), TEXT("Charlie"), TEXT("Diana"), TEXT("Elena"), TEXT("Frank") };
		P->Name = Marker + TEXT("-") + Names[i];
		P->Level = 1;
	});

	TSharedPtr<FFilterResult> Result = MakeShared<FFilterResult>();
	AddFilterQueryStep(this, TEXT("Query"),
		FPlayServFilter::Where(TEXT("Name")).NotEqualTo(Marker + TEXT("-Charlie")), Result);

	AddCommand(new FPlayServAssertStep(this, [Result, Marker](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("NotEqualTo(\"Charlie\") count (this run's rows)"), Result->CountWithPrefix(Marker), 5);
		T->TestTrue(TEXT("Contains Alice"), Result->ContainsName(Marker + TEXT("-Alice")));
		T->TestTrue(TEXT("Contains Bob"), Result->ContainsName(Marker + TEXT("-Bob")));
		T->TestTrue(TEXT("Contains Diana"), Result->ContainsName(Marker + TEXT("-Diana")));
		T->TestTrue(TEXT("Contains Elena"), Result->ContainsName(Marker + TEXT("-Elena")));
		T->TestTrue(TEXT("Contains Frank"), Result->ContainsName(Marker + TEXT("-Frank")));
		T->TestFalse(TEXT("Excludes Charlie"), Result->ContainsName(Marker + TEXT("-Charlie")));
		Result->LogTo(T);
	}));

	AddCollectionCleanupStep(this);
	return true;
}

// 6 players, levels 5,25,42,42,80,100 — NotEqualTo(42) expects 4 of this run's rows
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServFilterNotEqualToIntTest,
	"PlayServ.Data.Collection.FilterNotEqualToInt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServFilterNotEqualToIntTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("filter-ne-int"));

	const FString Marker = FGuid::NewGuid().ToString();

	AddCreatePlayersStep(this, TEXT("Create"), 6, [Marker](UTestPlayer* P, int32 i)
	{
		const TCHAR* Names[] = { TEXT("L5"), TEXT("L25"), TEXT("L42a"), TEXT("L42b"), TEXT("L80"), TEXT("L100") };
		int32 Levels[] = { 5, 25, 42, 42, 80, 100 };
		P->Name = Marker + TEXT("-") + Names[i];
		P->Level = Levels[i];
	});

	TSharedPtr<FFilterResult> Result = MakeShared<FFilterResult>();
	AddFilterQueryStep(this, TEXT("Query"),
		FPlayServFilter::Where(TEXT("Level")).NotEqualTo(42), Result);

	AddCommand(new FPlayServAssertStep(this, [Result, Marker](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("NotEqualTo(42) count (this run's rows)"), Result->CountWithPrefix(Marker), 4);
		T->TestTrue(TEXT("Contains L5"), Result->ContainsName(Marker + TEXT("-L5")));
		T->TestTrue(TEXT("Contains L25"), Result->ContainsName(Marker + TEXT("-L25")));
		T->TestTrue(TEXT("Contains L80"), Result->ContainsName(Marker + TEXT("-L80")));
		T->TestTrue(TEXT("Contains L100"), Result->ContainsName(Marker + TEXT("-L100")));
		T->TestFalse(TEXT("Excludes L42a"), Result->ContainsName(Marker + TEXT("-L42a")));
		T->TestFalse(TEXT("Excludes L42b"), Result->ContainsName(Marker + TEXT("-L42b")));
		Result->LogTo(T);
	}));

	AddCollectionCleanupStep(this);
	return true;
}

// 6 players, levels 3,5,20,42,80,100 — GreaterThan(20) expects 3, boundary excluded
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServFilterGreaterThanTest,
	"PlayServ.Data.Collection.FilterGreaterThan",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServFilterGreaterThanTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("filter-gt"));

	const FString Marker = FGuid::NewGuid().ToString();

	AddCreatePlayersStep(this, TEXT("Create"), 6, [Marker](UTestPlayer* P, int32 i)
	{
		const TCHAR* Names[] = { TEXT("L3"), TEXT("L5"), TEXT("Boundary20"), TEXT("L42"), TEXT("L80"), TEXT("L100") };
		int32 Levels[] = { 3, 5, 20, 42, 80, 100 };
		P->Name = Marker + TEXT("-") + Names[i];
		P->Level = Levels[i];
	});

	TSharedPtr<FFilterResult> Result = MakeShared<FFilterResult>();
	AddFilterQueryStep(this, TEXT("Query"),
		FPlayServFilter::Where(TEXT("Level")).GreaterThan(20), Result);

	AddCommand(new FPlayServAssertStep(this, [Result, Marker](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("GreaterThan(20) count (this run's rows)"), Result->CountWithPrefix(Marker), 3);
		T->TestTrue(TEXT("Contains L42"), Result->ContainsName(Marker + TEXT("-L42")));
		T->TestTrue(TEXT("Contains L80"), Result->ContainsName(Marker + TEXT("-L80")));
		T->TestTrue(TEXT("Contains L100"), Result->ContainsName(Marker + TEXT("-L100")));
		T->TestFalse(TEXT("Excludes L3"), Result->ContainsName(Marker + TEXT("-L3")));
		T->TestFalse(TEXT("Excludes L5"), Result->ContainsName(Marker + TEXT("-L5")));
		T->TestFalse(TEXT("Excludes Boundary20 (exclusive)"), Result->ContainsName(Marker + TEXT("-Boundary20")));
		Result->LogTo(T);
	}));

	AddCollectionCleanupStep(this);
	return true;
}

// 6 players, levels 3,5,42,42,80,100 — GreaterThanOrEqual(42) expects 4, boundary included
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServFilterGreaterThanOrEqualTest,
	"PlayServ.Data.Collection.FilterGreaterThanOrEqual",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServFilterGreaterThanOrEqualTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("filter-gte"));

	const FString Marker = FGuid::NewGuid().ToString();

	AddCreatePlayersStep(this, TEXT("Create"), 6, [Marker](UTestPlayer* P, int32 i)
	{
		const TCHAR* Names[] = { TEXT("L3"), TEXT("L5"), TEXT("Boundary42a"), TEXT("Boundary42b"), TEXT("L80"), TEXT("L100") };
		int32 Levels[] = { 3, 5, 42, 42, 80, 100 };
		P->Name = Marker + TEXT("-") + Names[i];
		P->Level = Levels[i];
	});

	TSharedPtr<FFilterResult> Result = MakeShared<FFilterResult>();
	AddFilterQueryStep(this, TEXT("Query"),
		FPlayServFilter::Where(TEXT("Level")).GreaterThanOrEqual(42), Result);

	AddCommand(new FPlayServAssertStep(this, [Result, Marker](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("GreaterThanOrEqual(42) count (this run's rows)"), Result->CountWithPrefix(Marker), 4);
		T->TestTrue(TEXT("Contains Boundary42a (inclusive)"), Result->ContainsName(Marker + TEXT("-Boundary42a")));
		T->TestTrue(TEXT("Contains Boundary42b (inclusive)"), Result->ContainsName(Marker + TEXT("-Boundary42b")));
		T->TestTrue(TEXT("Contains L80"), Result->ContainsName(Marker + TEXT("-L80")));
		T->TestTrue(TEXT("Contains L100"), Result->ContainsName(Marker + TEXT("-L100")));
		T->TestFalse(TEXT("Excludes L3"), Result->ContainsName(Marker + TEXT("-L3")));
		T->TestFalse(TEXT("Excludes L5"), Result->ContainsName(Marker + TEXT("-L5")));
		Result->LogTo(T);
	}));

	AddCollectionCleanupStep(this);
	return true;
}

// 6 players, levels 3,5,20,42,80,100 — LessThan(42) expects 3, boundary excluded
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServFilterLessThanTest,
	"PlayServ.Data.Collection.FilterLessThan",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServFilterLessThanTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("filter-lt"));

	const FString Marker = FGuid::NewGuid().ToString();

	AddCreatePlayersStep(this, TEXT("Create"), 6, [Marker](UTestPlayer* P, int32 i)
	{
		const TCHAR* Names[] = { TEXT("L3"), TEXT("L5"), TEXT("L20"), TEXT("Boundary42"), TEXT("L80"), TEXT("L100") };
		int32 Levels[] = { 3, 5, 20, 42, 80, 100 };
		P->Name = Marker + TEXT("-") + Names[i];
		P->Level = Levels[i];
	});

	TSharedPtr<FFilterResult> Result = MakeShared<FFilterResult>();
	AddFilterQueryStep(this, TEXT("Query"),
		FPlayServFilter::Where(TEXT("Level")).LessThan(42), Result);

	AddCommand(new FPlayServAssertStep(this, [Result, Marker](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("LessThan(42) count (this run's rows)"), Result->CountWithPrefix(Marker), 3);
		T->TestTrue(TEXT("Contains L3"), Result->ContainsName(Marker + TEXT("-L3")));
		T->TestTrue(TEXT("Contains L5"), Result->ContainsName(Marker + TEXT("-L5")));
		T->TestTrue(TEXT("Contains L20"), Result->ContainsName(Marker + TEXT("-L20")));
		T->TestFalse(TEXT("Excludes Boundary42 (exclusive)"), Result->ContainsName(Marker + TEXT("-Boundary42")));
		T->TestFalse(TEXT("Excludes L80"), Result->ContainsName(Marker + TEXT("-L80")));
		T->TestFalse(TEXT("Excludes L100"), Result->ContainsName(Marker + TEXT("-L100")));
		Result->LogTo(T);
	}));

	AddCollectionCleanupStep(this);
	return true;
}

// 6 players, levels 3,5,42,42,80,100 — LessThanOrEqual(42) expects 4, boundary included
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServFilterLessThanOrEqualTest,
	"PlayServ.Data.Collection.FilterLessThanOrEqual",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServFilterLessThanOrEqualTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("filter-lte"));

	const FString Marker = FGuid::NewGuid().ToString();

	AddCreatePlayersStep(this, TEXT("Create"), 6, [Marker](UTestPlayer* P, int32 i)
	{
		const TCHAR* Names[] = { TEXT("L3"), TEXT("L5"), TEXT("Boundary42a"), TEXT("Boundary42b"), TEXT("L80"), TEXT("L100") };
		int32 Levels[] = { 3, 5, 42, 42, 80, 100 };
		P->Name = Marker + TEXT("-") + Names[i];
		P->Level = Levels[i];
	});

	TSharedPtr<FFilterResult> Result = MakeShared<FFilterResult>();
	AddFilterQueryStep(this, TEXT("Query"),
		FPlayServFilter::Where(TEXT("Level")).LessThanOrEqual(42), Result);

	AddCommand(new FPlayServAssertStep(this, [Result, Marker](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("LessThanOrEqual(42) count (this run's rows)"), Result->CountWithPrefix(Marker), 4);
		T->TestTrue(TEXT("Contains L3"), Result->ContainsName(Marker + TEXT("-L3")));
		T->TestTrue(TEXT("Contains L5"), Result->ContainsName(Marker + TEXT("-L5")));
		T->TestTrue(TEXT("Contains Boundary42a (inclusive)"), Result->ContainsName(Marker + TEXT("-Boundary42a")));
		T->TestTrue(TEXT("Contains Boundary42b (inclusive)"), Result->ContainsName(Marker + TEXT("-Boundary42b")));
		T->TestFalse(TEXT("Excludes L80"), Result->ContainsName(Marker + TEXT("-L80")));
		T->TestFalse(TEXT("Excludes L100"), Result->ContainsName(Marker + TEXT("-L100")));
		Result->LogTo(T);
	}));

	AddCollectionCleanupStep(this);
	return true;
}

// 6 players — In({Alice,Diana,Frank}) expects 3 (values are marker-qualified, so the exact-match
// In query is marker-scoped and the raw count is deterministic)
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServFilterInArrayTest,
	"PlayServ.Data.Collection.FilterInArray",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServFilterInArrayTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("filter-in"));

	const FString Marker = FGuid::NewGuid().ToString();

	AddCreatePlayersStep(this, TEXT("Create"), 6, [Marker](UTestPlayer* P, int32 i)
	{
		const TCHAR* Names[] = { TEXT("Alice"), TEXT("Bob"), TEXT("Charlie"), TEXT("Diana"), TEXT("Elena"), TEXT("Frank") };
		P->Name = Marker + TEXT("-") + Names[i];
		P->Level = 1;
	});

	TSharedPtr<FFilterResult> Result = MakeShared<FFilterResult>();
	TArray<FString> InValues = { Marker + TEXT("-Alice"), Marker + TEXT("-Diana"), Marker + TEXT("-Frank") };
	AddFilterQueryStep(this, TEXT("Query"),
		FPlayServFilter::Where(TEXT("Name")).In(InValues), Result);

	AddCommand(new FPlayServAssertStep(this, [Result, Marker](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("In({Alice,Diana,Frank}) count"), Result->Count, 3);
		T->TestTrue(TEXT("Contains Alice"), Result->ContainsName(Marker + TEXT("-Alice")));
		T->TestTrue(TEXT("Contains Diana"), Result->ContainsName(Marker + TEXT("-Diana")));
		T->TestTrue(TEXT("Contains Frank"), Result->ContainsName(Marker + TEXT("-Frank")));
		T->TestFalse(TEXT("Excludes Bob"), Result->ContainsName(Marker + TEXT("-Bob")));
		T->TestFalse(TEXT("Excludes Charlie"), Result->ContainsName(Marker + TEXT("-Charlie")));
		T->TestFalse(TEXT("Excludes Elena"), Result->ContainsName(Marker + TEXT("-Elena")));
		Result->LogTo(T);
	}));

	AddCollectionCleanupStep(this);
	return true;
}

// 6 players: 3 with clan, 3 without — IsNull expects 3 of this run's rows
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServFilterIsNullTest,
	"PlayServ.Data.Collection.FilterIsNull",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServFilterIsNullTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("filter-isnull"));

	const FString Marker = FGuid::NewGuid().ToString();

	TArray<FString> WithNames = { Marker + TEXT("-WithClan1"), Marker + TEXT("-WithClan2"), Marker + TEXT("-WithClan3") };
	TArray<FString> NoNames = { Marker + TEXT("-NoClan1"), Marker + TEXT("-NoClan2"), Marker + TEXT("-NoClan3") };
	AddCreateClanAndPlayersStep(this, TEXT("NullTestClan"), WithNames, NoNames);

	TSharedPtr<FFilterResult> Result = MakeShared<FFilterResult>();
	AddFilterQueryStep(this, TEXT("Query"),
		FPlayServFilter::Where(TEXT("Clan")).IsNull(), Result);

	AddCommand(new FPlayServAssertStep(this, [Result, Marker](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("IsNull on Clan count (this run's rows)"), Result->CountWithPrefix(Marker), 3);
		T->TestTrue(TEXT("Contains NoClan1"), Result->ContainsName(Marker + TEXT("-NoClan1")));
		T->TestTrue(TEXT("Contains NoClan2"), Result->ContainsName(Marker + TEXT("-NoClan2")));
		T->TestTrue(TEXT("Contains NoClan3"), Result->ContainsName(Marker + TEXT("-NoClan3")));
		T->TestFalse(TEXT("Excludes WithClan1"), Result->ContainsName(Marker + TEXT("-WithClan1")));
		T->TestFalse(TEXT("Excludes WithClan2"), Result->ContainsName(Marker + TEXT("-WithClan2")));
		T->TestFalse(TEXT("Excludes WithClan3"), Result->ContainsName(Marker + TEXT("-WithClan3")));
		Result->LogTo(T);
	}));

	AddCollectionCleanupStep(this);
	return true;
}

// 6 players: 3 with clan, 3 without — IsNotNull expects 3 of this run's rows
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServFilterIsNotNullTest,
	"PlayServ.Data.Collection.FilterIsNotNull",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServFilterIsNotNullTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("filter-isnotnull"));

	const FString Marker = FGuid::NewGuid().ToString();

	TArray<FString> WithNames = { Marker + TEXT("-WithClan1"), Marker + TEXT("-WithClan2"), Marker + TEXT("-WithClan3") };
	TArray<FString> NoNames = { Marker + TEXT("-NoClan1"), Marker + TEXT("-NoClan2"), Marker + TEXT("-NoClan3") };
	AddCreateClanAndPlayersStep(this, TEXT("NotNullTestClan"), WithNames, NoNames);

	TSharedPtr<FFilterResult> Result = MakeShared<FFilterResult>();
	AddFilterQueryStep(this, TEXT("Query"),
		FPlayServFilter::Where(TEXT("Clan")).IsNotNull(), Result);

	AddCommand(new FPlayServAssertStep(this, [Result, Marker](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("IsNotNull on Clan count (this run's rows)"), Result->CountWithPrefix(Marker), 3);
		T->TestTrue(TEXT("Contains WithClan1"), Result->ContainsName(Marker + TEXT("-WithClan1")));
		T->TestTrue(TEXT("Contains WithClan2"), Result->ContainsName(Marker + TEXT("-WithClan2")));
		T->TestTrue(TEXT("Contains WithClan3"), Result->ContainsName(Marker + TEXT("-WithClan3")));
		T->TestFalse(TEXT("Excludes NoClan1"), Result->ContainsName(Marker + TEXT("-NoClan1")));
		T->TestFalse(TEXT("Excludes NoClan2"), Result->ContainsName(Marker + TEXT("-NoClan2")));
		T->TestFalse(TEXT("Excludes NoClan3"), Result->ContainsName(Marker + TEXT("-NoClan3")));
		Result->LogTo(T);
	}));

	AddCollectionCleanupStep(this);
	return true;
}

// Dotted (nested) filter paths are ADDRESSABLE: every segment but the last names an inclusion
// field, the last names a field the referenced part declares, segments match ordinal-exact, and
// a path is at most 8 segments deep (contracts/conventions.md §18). The SDK passes the path
// through verbatim; the platform resolves it into the embedded part. This canary pins the
// acceptance end to end so a platform regression to refusing dotted paths fails loudly here.
// The other half of the grammar — a path onto no declared field is still 422 — is FilterUndeclaredNestedPathRejected.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServFilterNestedPathAcceptedTest,
	"PlayServ.Data.Collection.FilterNestedPathAccepted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServFilterNestedPathAcceptedTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("filter-nested-path"));

	const FString Marker = FGuid::NewGuid().ToString();

	// Young:  Profile.Age=20 — excluded
	// Adult:  Profile.Age=30 — MATCH
	// Senior: Profile.Age=40 — MATCH
	AddCreatePlayersStep(this, TEXT("Create"), 3, [Marker](UTestPlayer* P, int32 i)
	{
		struct FSetup
		{
			const TCHAR* Name;
			int32 Age;
		};
		FSetup Data[] = {
			{ TEXT("Young"),  20 },
			{ TEXT("Adult"),  30 },
			{ TEXT("Senior"), 40 },
		};
		P->Name = Marker + TEXT("-") + Data[i].Name;
		P->Profile.Age = Data[i].Age;
		P->Profile.Bio = FString::Printf(TEXT("age %d"), Data[i].Age);
	});

	// Captures the outcome as well as the rows: a regression to the old 422 must read as
	// "rejected: ValidationFailed/validation_failed", not as an unexplained count of 0.
	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	TSharedPtr<bool> bSuccess = MakeShared<bool>(false);
	TSharedPtr<EPlayServErrorCode> Code = MakeShared<EPlayServErrorCode>(EPlayServErrorCode::None);
	TSharedPtr<FString> ProblemCode = MakeShared<FString>();
	TSharedPtr<FFilterResult> Result = MakeShared<FFilterResult>();
	AddCommand(new FPlayServAsyncStep(this, TEXT("QueryDottedPath"), [bDone, bSuccess, Code, ProblemCode, Result]()
	{
		PlayServ::Data::LoadAll<UTestPlayer>(FPlayServFilter::Where(TEXT("Profile.Age")).GreaterThan(25),
			[bDone, bSuccess, Code, ProblemCode, Result](bool bOk, TArray<UTestPlayer*> Results, const FPlayServError& Error)
		{
			*bSuccess = bOk;
			*Code = Error.Code;
			*ProblemCode = Error.ProblemCode;
			if (bOk)
			{
				Result->Count = Results.Num();
				for (UTestPlayer* P : Results)
				{
					if (P)
					{
						Result->Names.Add(P->Name);
					}
				}
			}
			*bDone = true;
		});
	}, bDone, 3.0f));

	AddCommand(new FPlayServAssertStep(this, [bSuccess, Code, ProblemCode, Result, Marker](FAutomationTestBase* T)
	{
		T->TestTrue(FString::Printf(TEXT("Dotted filter path is accepted (got code=%d problem='%s')"),
			static_cast<int32>(*Code), **ProblemCode), *bSuccess);
		T->TestEqual(TEXT("Profile.Age > 25 count (this run's rows)"), Result->CountWithPrefix(Marker), 2);
		T->TestTrue(TEXT("Contains Adult (Profile.Age=30)"), Result->ContainsName(Marker + TEXT("-Adult")));
		T->TestTrue(TEXT("Contains Senior (Profile.Age=40)"), Result->ContainsName(Marker + TEXT("-Senior")));
		T->TestFalse(TEXT("Excludes Young (Profile.Age=20)"), Result->ContainsName(Marker + TEXT("-Young")));
		Result->LogTo(T);
	}));

	AddCollectionCleanupStep(this);
	return true;
}

// The other half of the nested-path grammar: a dotted path that lands on no declared field is
// still refused 422 validation_failed — never a silently dropped clause (contracts/conventions.md
// §18). Profile is a declared inclusion on TestPlayer; DoesNotExist is not a field of the part
// it references. Together with FilterNestedPathAccepted this pins both edges of the rule.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServFilterUndeclaredNestedPathRejectedTest,
	"PlayServ.Data.Collection.FilterUndeclaredNestedPathRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServFilterUndeclaredNestedPathRejectedTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("filter-undeclared-nested-path"));

	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	TSharedPtr<bool> bSuccess = MakeShared<bool>(true);
	TSharedPtr<EPlayServErrorCode> Code = MakeShared<EPlayServErrorCode>(EPlayServErrorCode::None);
	TSharedPtr<FString> ProblemCode = MakeShared<FString>();
	AddCommand(new FPlayServAsyncStep(this, TEXT("QueryUndeclaredDottedPath"), [bDone, bSuccess, Code, ProblemCode]()
	{
		PlayServ::Data::LoadAll<UTestPlayer>(FPlayServFilter::Where(TEXT("Profile.DoesNotExist")).GreaterThan(25),
			[bDone, bSuccess, Code, ProblemCode](bool bOk, TArray<UTestPlayer*>, const FPlayServError& Error)
		{
			*bSuccess = bOk;
			*Code = Error.Code;
			*ProblemCode = Error.ProblemCode;
			*bDone = true;
		});
	}, bDone, 3.0f));

	AddCommand(new FPlayServAssertStep(this, [bSuccess, Code, ProblemCode](FAutomationTestBase* T)
	{
		T->TestFalse(TEXT("Undeclared dotted filter path is rejected"), *bSuccess);
		T->TestEqual(TEXT("422 maps to ValidationFailed"), *Code, EPlayServErrorCode::ValidationFailed);
		T->TestEqual(TEXT("ProblemCode is validation_failed"), *ProblemCode, FString(TEXT("validation_failed")));
	}));

	return true;
}

// Seeds marker-tagged players and captures each one's server-minted id by short name, so the
// id-batch tests below can address rows the way a caller would — by rec_* id, not by filter.
static void AddCreatePlayersCapturingIdsStep(
	FAutomationTestBase* Test,
	const FString& StepName,
	const FString& Marker,
	TArray<TPair<FString, int32>> NameAndLevel,
	TSharedPtr<TMap<FString, FString>> OutIdsByName)
{
	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	Test->AddCommand(new FPlayServAsyncStep(Test, StepName, [bDone, Marker, NameAndLevel, OutIdsByName]()
	{
		UPlayServSubsystem* PS = UPlayServSubsystem::Get();
		if (!PS) { *bDone = true; return; }
		TArray<UObject*> Batch;
		TArray<TPair<FString, TWeakObjectPtr<UTestPlayer>>> Created;
		for (const TPair<FString, int32>& Entry : NameAndLevel)
		{
			UTestPlayer* P = PlayServ::Data::Create<UTestPlayer>();
			P->Name = Marker + TEXT("-") + Entry.Key;
			P->Level = Entry.Value;
			Batch.Add(P);
			Created.Emplace(Entry.Key, TWeakObjectPtr<UTestPlayer>(P));
		}
		PlayServ::Data::BulkSave(Batch, FPlayServSimpleCallback::CreateLambda(
			[bDone, Created, OutIdsByName](bool, const FPlayServError&)
			{
				for (const TPair<FString, TWeakObjectPtr<UTestPlayer>>& Entry : Created)
				{
					if (UTestPlayer* P = Entry.Value.Get())
					{
						OutIdsByName->Add(Entry.Key, UPlayServData::GetRecordId(P));
					}
				}
				*bDone = true;
			}));
	}, bDone, 3.0f));
}

// Runs the low-level id-batch read and captures the outcome plus the returned Names in the
// order the platform tier delivered them.
static void AddIdBatchQueryStep(
	FAutomationTestBase* Test,
	const FString& StepName,
	TFunction<TArray<FString>()> IdsProvider,
	FPlayServFilter Filter,
	TSharedPtr<bool> OutSuccess,
	TSharedPtr<FString> OutErrorMessage,
	TSharedPtr<TArray<FString>> OutNames)
{
	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	Test->AddCommand(new FPlayServAsyncStep(Test, StepName,
		[bDone, IdsProvider = MoveTemp(IdsProvider), Filter = MoveTemp(Filter), OutSuccess, OutErrorMessage, OutNames]()
	{
		UPlayServSubsystem* PS = UPlayServSubsystem::Get();
		if (!PS) { *bDone = true; return; }
		PS->GetData()->GetAll(UTestPlayer::StaticClass()->GetName(), IdsProvider(), Filter,
			FPlayServJsonCallback::CreateLambda([bDone, OutSuccess, OutErrorMessage, OutNames](bool bOk, const TSharedPtr<FJsonObject>& Result, const FPlayServError& Error)
			{
				*OutSuccess = bOk;
				*OutErrorMessage = Error.Message;
				const TArray<TSharedPtr<FJsonValue>>* Items;
				if (bOk && Result.IsValid() && Result->TryGetArrayField(TEXT("items"), Items))
				{
					for (const TSharedPtr<FJsonValue>& Item : *Items)
					{
						const TSharedPtr<FJsonObject>* Obj;
						FString Name;
						if (Item.IsValid() && Item->TryGetObject(Obj) && (*Obj)->TryGetStringField(TEXT("Name"), Name))
						{
							OutNames->Add(Name);
						}
					}
				}
				*bDone = true;
			}));
	}, bDone, 3.0f));
}

// Ids AND a filter apply together in ONE records:query. Seeds A (Level 5) and B/C/D (Level 50); asks for ids {A, B, C} with Level == 50 and
// expects exactly [B, C]. The assertions are shaped so that dropping either half fails loudly:
// D present → the id half was dropped; A present → the filter half was dropped — so a refactor
// cannot reintroduce silent dropping and stay green.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServIdBatchWithFilterAppliesBothTest,
	"PlayServ.Data.Collection.IdBatchWithFilterAppliesBoth",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServIdBatchWithFilterAppliesBothTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("id-batch-with-filter"));

	const FString Marker = FGuid::NewGuid().ToString();
	TSharedPtr<TMap<FString, FString>> IdsByName = MakeShared<TMap<FString, FString>>();
	AddCreatePlayersCapturingIdsStep(this, TEXT("Create"), Marker,
		{ { TEXT("A"), 5 }, { TEXT("B"), 50 }, { TEXT("C"), 50 }, { TEXT("D"), 50 } }, IdsByName);

	TSharedPtr<bool> bSuccess = MakeShared<bool>(false);
	TSharedPtr<FString> ErrorMessage = MakeShared<FString>();
	TSharedPtr<TArray<FString>> Names = MakeShared<TArray<FString>>();
	AddIdBatchQueryStep(this, TEXT("QueryIdsWithFilter"),
		[IdsByName]() { return TArray<FString>{ IdsByName->FindRef(TEXT("A")), IdsByName->FindRef(TEXT("B")), IdsByName->FindRef(TEXT("C")) }; },
		FPlayServFilter::Where(TEXT("Level")).EqualTo(50),
		bSuccess, ErrorMessage, Names);

	AddCommand(new FPlayServAssertStep(this, [bSuccess, ErrorMessage, Names, Marker](FAutomationTestBase* T)
	{
		T->TestTrue(FString::Printf(TEXT("ids + filter is accepted (error: '%s')"), **ErrorMessage), *bSuccess);
		T->TestEqual(TEXT("Exactly the rows in BOTH the id batch and the filter"), Names->Num(), 2);
		T->TestTrue(TEXT("Contains B (in ids, Level 50)"), Names->Contains(Marker + TEXT("-B")));
		T->TestTrue(TEXT("Contains C (in ids, Level 50)"), Names->Contains(Marker + TEXT("-C")));
		T->TestFalse(TEXT("Excludes A (in ids but Level 5 — the filter half must apply)"), Names->Contains(Marker + TEXT("-A")));
		T->TestFalse(TEXT("Excludes D (Level 50 but not in ids — the id half must apply)"), Names->Contains(Marker + TEXT("-D")));
		if (Names->Num() == 2)
		{
			T->TestEqual(TEXT("Request order kept: B before C"), (*Names)[0], Marker + TEXT("-B"));
		}
		T->AddInfo(FString::Printf(TEXT("Returned: [%s]"), *FString::Join(*Names, TEXT(", "))));
	}));

	AddCollectionCleanupStep(this);
	return true;
}

// The documented id-batch shape over one records:query: rows come back in REQUEST order, an
// unknown id is skipped rather than failing the batch, and a repeated id is answered once per
// occurrence — re-derived client-side from the query's rows (the server returns them in its own
// order).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServIdBatchPreservesOrderAndSkipsMissingTest,
	"PlayServ.Data.Collection.IdBatchPreservesOrderAndSkipsMissing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServIdBatchPreservesOrderAndSkipsMissingTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("id-batch-order"));

	const FString Marker = FGuid::NewGuid().ToString();
	TSharedPtr<TMap<FString, FString>> IdsByName = MakeShared<TMap<FString, FString>>();
	AddCreatePlayersCapturingIdsStep(this, TEXT("Create"), Marker,
		{ { TEXT("A"), 1 }, { TEXT("B"), 2 }, { TEXT("C"), 3 } }, IdsByName);

	TSharedPtr<bool> bSuccess = MakeShared<bool>(false);
	TSharedPtr<FString> ErrorMessage = MakeShared<FString>();
	TSharedPtr<TArray<FString>> Names = MakeShared<TArray<FString>>();
	AddIdBatchQueryStep(this, TEXT("QueryIdsOutOfOrder"),
		[IdsByName]()
		{
			return TArray<FString>{
				IdsByName->FindRef(TEXT("C")),
				TEXT("rec_00000000000000000000000000"),
				IdsByName->FindRef(TEXT("A")),
				IdsByName->FindRef(TEXT("C")) };
		},
		FPlayServFilter::None(),
		bSuccess, ErrorMessage, Names);

	AddCommand(new FPlayServAssertStep(this, [bSuccess, ErrorMessage, Names, Marker](FAutomationTestBase* T)
	{
		T->TestTrue(FString::Printf(TEXT("id batch succeeds with an unknown id in it (error: '%s')"), **ErrorMessage), *bSuccess);
		T->TestEqual(TEXT("Unknown id skipped, repeated id answered twice: 3 rows"), Names->Num(), 3);
		if (Names->Num() == 3)
		{
			T->TestEqual(TEXT("Request order [0] = C"), (*Names)[0], Marker + TEXT("-C"));
			T->TestEqual(TEXT("Request order [1] = A"), (*Names)[1], Marker + TEXT("-A"));
			T->TestEqual(TEXT("Request order [2] = C again"), (*Names)[2], Marker + TEXT("-C"));
		}
		T->AddInfo(FString::Printf(TEXT("Returned: [%s]"), *FString::Join(*Names, TEXT(", "))));
	}));

	AddCollectionCleanupStep(this);
	return true;
}

// 6 players — compound AND on top-level fields: Level>=10 AND active AND Score<40 expects 2
// (Charlie, Elena). Top-level fields only, on purpose: FilterNestedPathAccepted exercises the
// dotted-path grammar, so the compound-AND assertion does not depend on the nested-path one.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServFilterCompoundTest,
	"PlayServ.Data.Collection.FilterCompound",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServFilterCompoundTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("filter-compound"));

	const FString Marker = FGuid::NewGuid().ToString();

	// Alice:   L=5,   active=true,  Score=20  — excluded: Level < 10
	// Bob:     L=42,  active=false, Score=30  — excluded: inactive
	// Charlie: L=42,  active=true,  Score=25  — MATCH
	// Diana:   L=80,  active=true,  Score=45  — excluded: Score >= 40
	// Elena:   L=15,  active=true,  Score=30  — MATCH
	// Frank:   L=100, active=true,  Score=50  — excluded: Score >= 40
	AddCreatePlayersStep(this, TEXT("Create"), 6, [Marker](UTestPlayer* P, int32 i)
	{
		struct FSetup
		{
			const TCHAR* Name;
			int32 Level;
			bool bActive;
			float Score;
		};
		FSetup Data[] = {
			{ TEXT("Alice"),   5,   true,  20.0f },
			{ TEXT("Bob"),     42,  false, 30.0f },
			{ TEXT("Charlie"), 42,  true,  25.0f },
			{ TEXT("Diana"),   80,  true,  45.0f },
			{ TEXT("Elena"),   15,  true,  30.0f },
			{ TEXT("Frank"),   100, true,  50.0f },
		};
		P->Name = Marker + TEXT("-") + Data[i].Name;
		P->Level = Data[i].Level;
		P->bActive = Data[i].bActive;
		P->Score = Data[i].Score;
	});

	TSharedPtr<FFilterResult> Result = MakeShared<FFilterResult>();
	AddFilterQueryStep(this, TEXT("Query"),
		FPlayServFilter::Where(TEXT("Level")).GreaterThanOrEqual(10)
			.And(TEXT("bActive")).EqualTo(true)
			.And(TEXT("Score")).LessThan(40),
		Result);

	AddCommand(new FPlayServAssertStep(this, [Result, Marker](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("Compound filter count (this run's rows)"), Result->CountWithPrefix(Marker), 2);
		T->TestTrue(TEXT("Contains Charlie (L=42, active, Score=25)"), Result->ContainsName(Marker + TEXT("-Charlie")));
		T->TestTrue(TEXT("Contains Elena (L=15, active, Score=30)"), Result->ContainsName(Marker + TEXT("-Elena")));
		T->TestFalse(TEXT("Excludes Alice (Level < 10)"), Result->ContainsName(Marker + TEXT("-Alice")));
		T->TestFalse(TEXT("Excludes Bob (inactive)"), Result->ContainsName(Marker + TEXT("-Bob")));
		T->TestFalse(TEXT("Excludes Diana (Score >= 40)"), Result->ContainsName(Marker + TEXT("-Diana")));
		T->TestFalse(TEXT("Excludes Frank (Score >= 40)"), Result->ContainsName(Marker + TEXT("-Frank")));
		Result->LogTo(T);
	}));

	AddCollectionCleanupStep(this);
	return true;
}

// Load All / Delete All

// Create 3 players, LoadAll with None() filter, verify all returned.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServCollectionLoadAllUnfilteredTest,
	"PlayServ.Data.Collection.LoadAllUnfiltered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServCollectionLoadAllUnfilteredTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("coll-load-all"));

	AddCreatePlayersStep(this, TEXT("Create"), 3, [](UTestPlayer* P, int32 i)
	{
		P->Name = FString::Printf(TEXT("UnfilteredPlayer_%d"), i);
		P->Level = (i + 1) * 10;
		P->Score = static_cast<float>((i + 1) * 100);
	});

	TSharedPtr<bool> bQueryDone = MakeShared<bool>(false);
	TSharedPtr<int32> QueryCount = MakeShared<int32>(0);
	AddCommand(new FPlayServAsyncStep(this, TEXT("LoadAllNoFilter"), [bQueryDone, QueryCount]()
	{
		PlayServ::Data::LoadAll<UTestPlayer>(FPlayServFilter::None(), [bQueryDone, QueryCount](bool bSuccess, TArray<UTestPlayer*> Results, const FPlayServError&)
		{
			if (bSuccess)
			{
				*QueryCount = Results.Num();
			}
			*bQueryDone = true;
		});
	}, bQueryDone));

	AddCommand(new FPlayServAssertStep(this, [QueryCount](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("LoadAll returned >= 3 entities"), *QueryCount >= 3);
		T->AddInfo(FString::Printf(TEXT("LoadAll(None) returned %d entities"), *QueryCount));
	}));

	AddCollectionCleanupStep(this);
	return true;
}

// Create 3 players, DeleteAll with All(), verify 0 remaining. The dedicated wipe test — it
// keeps the wipe-based shape (leftover rows from other runs just raise DeletedCount).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServCollectionDeleteAllWithVerifyTest,
	"PlayServ.Data.Collection.DeleteAllWithVerify",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServCollectionDeleteAllWithVerifyTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("coll-delete-all"));

	AddCreatePlayersStep(this, TEXT("Create"), 3, [](UTestPlayer* P, int32 i)
	{
		P->Name = FString::Printf(TEXT("DeleteAllPlayer_%d"), i);
		P->Level = (i + 1) * 10;
	});

	TSharedPtr<bool> bDeleteAllDone = MakeShared<bool>(false);
	TSharedPtr<int32> DeleteCount = MakeShared<int32>(0);
	AddCommand(new FPlayServAsyncStep(this, TEXT("DeleteAll"), [bDeleteAllDone, DeleteCount]()
	{
		PlayServ::Data::DeleteAll<UTestPlayer>(FPlayServFilter::All(), FPlayServDeleteAllCallback::CreateLambda(
			[bDeleteAllDone, DeleteCount](bool bSuccess, const FPlayServDeleteAllResult& Result, const FPlayServError&)
			{
				if (bSuccess)
				{
					*DeleteCount = Result.DeletedCount;
				}
				*bDeleteAllDone = true;
			}));
	}, bDeleteAllDone, 15.0f));

	TSharedPtr<bool> bVerifyDone = MakeShared<bool>(false);
	TSharedPtr<int32> RemainingCount = MakeShared<int32>(-1);
	AddCommand(new FPlayServAsyncStep(this, TEXT("VerifyEmpty"), [bVerifyDone, RemainingCount]()
	{
		PlayServ::Data::LoadAll<UTestPlayer>(FPlayServFilter::None(), [bVerifyDone, RemainingCount](bool bSuccess, TArray<UTestPlayer*> Results, const FPlayServError&)
		{
			if (bSuccess)
			{
				*RemainingCount = Results.Num();
			}
			*bVerifyDone = true;
		});
	}, bVerifyDone));

	AddCommand(new FPlayServAssertStep(this, [DeleteCount, RemainingCount](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("DeleteAll removed entities"), *DeleteCount >= 3);
		T->TestEqual(TEXT("No entities remaining after DeleteAll"), *RemainingCount, 0);
		T->AddInfo(FString::Printf(TEXT("DeleteAll removed %d, remaining %d"), *DeleteCount, *RemainingCount));
	}));

	AddCollectionCleanupStep(this);
	return true;
}

#endif // !UE_BUILD_SHIPPING
