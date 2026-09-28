#include "PlayServTestCommon.h"
#include "Core/PlayServSubsystem.h"
#include "Auth/PlayServAuth.h"
#include "Data/PlayServData.h"
#include "PlayServ.h"
#include "Data/PlayServFilter.h"
#include "Core/PlayServSettings.h"
#include "TestEntities.h"

#if !UE_BUILD_SHIPPING

// A well-formed rec_* id the server never minted (ACL checks fire before row lookup, so this
// is also the safe probe id for tables we must never write to). File-unique name: this module
// builds unity, like the per-file cleanup helpers.
static const TCHAR* const ErrorMissingRecId = TEXT("rec_00000000000000000000000000");

// Cleanup helper — delete all error test entity types then logout. 15s: DeleteAll is
// query-then-fan-out on V2, so a leaked table from a crashed prior run can take a while.
static void AddErrorCleanupStep(FAutomationTestBase* Test)
{
	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	TSharedPtr<int32> Remaining = MakeShared<int32>(4);
	Test->AddCommand(new FPlayServAsyncStep(Test, TEXT("Cleanup"), [bDone, Remaining]()
	{
		auto OnDeleteDone = [bDone, Remaining]()
		{
			if (--(*Remaining) == 0)
			{
				// Cleanup no longer signs out — see PSV-2659. The EnsureLoggedOut step at the head
				// of UnauthenticatedRequestFails is unaffected: that one is an assertion, not cleanup.
				*bDone = true;
			}
		};

		PlayServ::Data::DeleteAll<UTestPlayer>(FPlayServFilter::All(), FPlayServDeleteAllCallback::CreateLambda(
			[OnDeleteDone](bool, const FPlayServDeleteAllResult&, const FPlayServError&)
			{
				OnDeleteDone();
			}));
		PlayServ::Data::DeleteAll<UTestClan>(FPlayServFilter::All(), FPlayServDeleteAllCallback::CreateLambda(
			[OnDeleteDone](bool, const FPlayServDeleteAllResult&, const FPlayServError&)
			{
				OnDeleteDone();
			}));
		PlayServ::Data::DeleteAll<UTestItem>(FPlayServFilter::All(), FPlayServDeleteAllCallback::CreateLambda(
			[OnDeleteDone](bool, const FPlayServDeleteAllResult&, const FPlayServError&)
			{
				OnDeleteDone();
			}));
		PlayServ::Data::DeleteAll<UTestInventory>(FPlayServFilter::All(), FPlayServDeleteAllCallback::CreateLambda(
			[OnDeleteDone](bool, const FPlayServDeleteAllResult&, const FPlayServError&)
			{
				OnDeleteDone();
			}));
	}, bDone, 15.0f));
}

// Network Failure

// Swap BaseURL to a non-routable address, attempt PSLoad, verify error callback.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServErrorNetworkFailureTest,
	"PlayServ.Data.ErrorHandling.NetworkFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServErrorNetworkFailureTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("error-network"));

	TSharedPtr<FString> OriginalURL = MakeShared<FString>();
	TSharedPtr<bool> bLoadDone = MakeShared<bool>(false);
	TSharedPtr<bool> bLoadSuccess = MakeShared<bool>(true);
	TSharedPtr<FString> ErrorMsg = MakeShared<FString>();
	TSharedPtr<EPlayServErrorCode> ErrorCode = MakeShared<EPlayServErrorCode>(EPlayServErrorCode::None);

	// Swap URL to non-routable address (RFC 5737 TEST-NET) and attempt PSLoad
	AddCommand(new FPlayServAsyncStep(this, TEXT("LoadWithBadURL"),
		[OriginalURL, bLoadDone, bLoadSuccess, ErrorMsg, ErrorCode]()
	{
		UPlayServSettings* Settings = GetMutableDefault<UPlayServSettings>();
		*OriginalURL = Settings->BaseURL;
		Settings->BaseURL = TEXT("http://localhost:1");

		PlayServ::Data::Load<UTestPlayer>(TEXT("nonexistent-id"),
			[bLoadDone, bLoadSuccess, ErrorMsg, ErrorCode](bool bSuccess, UTestPlayer*, const FPlayServError& Error)
		{
			*bLoadSuccess = bSuccess;
			*ErrorMsg = Error.Message;
			*ErrorCode = Error.Code;
			*bLoadDone = true;
		});
	}, bLoadDone, 10.0f));

	// Restore original URL — must always run regardless of test outcome
	TSharedPtr<bool> bRestoreDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("RestoreURL"),
		[bRestoreDone, OriginalURL]()
	{
		UPlayServSettings* Settings = GetMutableDefault<UPlayServSettings>();
		Settings->BaseURL = *OriginalURL;
		*bRestoreDone = true;
	}, bRestoreDone, 1.0f));

	AddCommand(new FPlayServAssertStep(this, [bLoadSuccess, ErrorMsg, ErrorCode](FAutomationTestBase* T)
	{
		T->TestFalse(TEXT("PSLoad failed with network error"), *bLoadSuccess);
		T->TestEqual(TEXT("Connection refusal maps to NetworkUnreachable"), *ErrorCode, EPlayServErrorCode::NetworkUnreachable);
		T->TestTrue(TEXT("Error mentions network/connect"),
			(*ErrorMsg).Contains(TEXT("Network")) || (*ErrorMsg).Contains(TEXT("connect")));
		T->AddInfo(FString::Printf(TEXT("Error: %s"), **ErrorMsg));
	}));

	AddErrorCleanupStep(this);
	return true;
}

// Timeout Classification

// Swap BaseURL to a blackhole address (unroutable, SYN silently dropped) with a short
// RequestTimeoutSeconds; the request must fail at ~the configured bound (not the engine
// default 30s+) with EPlayServErrorCode::Timeout. Runs without login — the request never
// reaches a backend, and skipping the live gateway keeps the test deterministic.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServErrorTimeoutClassificationTest,
	"PlayServ.Data.ErrorHandling.TimeoutClassification",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServErrorTimeoutClassificationTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));

	TSharedPtr<FString> OriginalURL = MakeShared<FString>();
	TSharedPtr<float> OriginalTimeout = MakeShared<float>(0.0f);
	TSharedPtr<bool> bLoadDone = MakeShared<bool>(false);
	TSharedPtr<bool> bLoadSuccess = MakeShared<bool>(true);
	TSharedPtr<FString> ErrorMsg = MakeShared<FString>();
	TSharedPtr<EPlayServErrorCode> ErrorCode = MakeShared<EPlayServErrorCode>(EPlayServErrorCode::None);
	TSharedPtr<double> StartTime = MakeShared<double>(0.0);
	TSharedPtr<double> ElapsedSecs = MakeShared<double>(0.0);

	AddCommand(new FPlayServAsyncStep(this, TEXT("LoadWithBlackholeURL"),
		[OriginalURL, OriginalTimeout, bLoadDone, bLoadSuccess, ErrorMsg, ErrorCode, StartTime, ElapsedSecs]()
	{
		UPlayServSettings* Settings = GetMutableDefault<UPlayServSettings>();
		*OriginalURL = Settings->BaseURL;
		*OriginalTimeout = Settings->RequestTimeoutSeconds;
		Settings->BaseURL = TEXT("https://10.255.255.1");
		Settings->RequestTimeoutSeconds = 3.0f;
		*StartTime = FPlatformTime::Seconds();

		PlayServ::Data::Load<UTestPlayer>(TEXT("nonexistent-id"),
			[bLoadDone, bLoadSuccess, ErrorMsg, ErrorCode, StartTime, ElapsedSecs](bool bSuccess, UTestPlayer*, const FPlayServError& Error)
		{
			*ElapsedSecs = FPlatformTime::Seconds() - *StartTime;
			*bLoadSuccess = bSuccess;
			*ErrorMsg = Error.Message;
			*ErrorCode = Error.Code;
			*bLoadDone = true;
		});
	}, bLoadDone, 15.0f));

	// Restore original settings — must always run regardless of test outcome
	TSharedPtr<bool> bRestoreDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("RestoreSettings"),
		[bRestoreDone, OriginalURL, OriginalTimeout]()
	{
		UPlayServSettings* Settings = GetMutableDefault<UPlayServSettings>();
		Settings->BaseURL = *OriginalURL;
		Settings->RequestTimeoutSeconds = *OriginalTimeout;
		*bRestoreDone = true;
	}, bRestoreDone, 1.0f));

	AddCommand(new FPlayServAssertStep(this, [bLoadSuccess, ErrorMsg, ErrorCode, ElapsedSecs](FAutomationTestBase* T)
	{
		T->TestFalse(TEXT("Load against blackhole address failed"), *bLoadSuccess);
		T->TestEqual(TEXT("Failure classifies as Timeout"), *ErrorCode, EPlayServErrorCode::Timeout);
		T->TestTrue(TEXT("Error message states the timeout"), (*ErrorMsg).Contains(TEXT("timed out")));
		T->TestTrue(FString::Printf(TEXT("Failed at ~RequestTimeoutSeconds, not the engine default (elapsed %.2fs)"), *ElapsedSecs),
			*ElapsedSecs >= 2.0 && *ElapsedSecs <= 10.0);
		T->AddInfo(FString::Printf(TEXT("Error: %s (%.2fs)"), **ErrorMsg, *ElapsedSecs));
	}));

	return true;
}

// Malformed Filter

// Filter on a nonexistent field name — the platform rejects it deterministically with 422,
// mapped to EPlayServErrorCode::ValidationFailed (the query plane validates filter fields
// against the schema).
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServErrorMalformedFilterTest,
	"PlayServ.Data.ErrorHandling.MalformedFilter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServErrorMalformedFilterTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("error-malformed"));

	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	TSharedPtr<bool> bQueryOk = MakeShared<bool>(true);
	TSharedPtr<EPlayServErrorCode> Code = MakeShared<EPlayServErrorCode>(EPlayServErrorCode::None);
	TSharedPtr<FString> ProblemCode = MakeShared<FString>();
	TSharedPtr<FString> ErrorMsg = MakeShared<FString>();
	AddCommand(new FPlayServAsyncStep(this, TEXT("QueryMalformed"),
		[bDone, bQueryOk, Code, ProblemCode, ErrorMsg]()
	{
		FPlayServFilter Filter = FPlayServFilter::Where(TEXT("NoSuchField")).EqualTo(1);
		PlayServ::Data::LoadAll<UTestPlayer>(Filter,
			[bDone, bQueryOk, Code, ProblemCode, ErrorMsg](bool bSuccess, TArray<UTestPlayer*>, const FPlayServError& Error)
		{
			*bQueryOk = bSuccess;
			*Code = Error.Code;
			*ProblemCode = Error.ProblemCode;
			*ErrorMsg = Error.Message;
			*bDone = true;
		});
	}, bDone, 5.0f));

	AddCommand(new FPlayServAssertStep(this, [bQueryOk, Code, ProblemCode, ErrorMsg](FAutomationTestBase* T)
	{
		T->TestFalse(TEXT("Filter on nonexistent field fails"), *bQueryOk);
		T->TestEqual(TEXT("422 maps to ValidationFailed"), *Code, EPlayServErrorCode::ValidationFailed);
		T->TestFalse(TEXT("ProblemCode carries the platform machine code"), (*ProblemCode).IsEmpty());
		T->AddInfo(FString::Printf(TEXT("ProblemCode=%s, Error=%s"), **ProblemCode, **ErrorMsg));
	}));

	AddErrorCleanupStep(this);
	return true;
}

// Logged-out ACL denial

// A logged-out data op does not fail as such: the test fixture tables are UNOWNED
// client-writable, so a pk_-only (logged-out) caller CAN read and write them by design. The
// logged-out denial therefore targets a client-CLOSED table —
// TestServerOnlyEntity — through the low-level string-typed tier (no game-module header needed; the ACL
// gate fires before row lookup, so a never-minted record id is safe). Expect 403 Forbidden with
// the platform machine code `table_read_forbidden` — NOT Unauthorized (401 stays reserved for
// unauthenticated-key problems) and NOT NotFound.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServErrorLoggedOutClosedTableForbiddenTest,
	"PlayServ.Data.ErrorHandling.LoggedOutClosedTableForbidden",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServErrorLoggedOutClosedTableForbiddenTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	// The subsystem is process-lifetime and may carry a session from a prior test in the same
	// run — log out explicitly so the request below runs on the bare pk_ plane.
	AddLogoutStep(this);

	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	TSharedPtr<bool> bSuccess = MakeShared<bool>(true);
	TSharedPtr<EPlayServErrorCode> Code = MakeShared<EPlayServErrorCode>(EPlayServErrorCode::None);
	TSharedPtr<FString> ProblemCode = MakeShared<FString>();
	AddCommand(new FPlayServAsyncStep(this, TEXT("ClosedTableRead"), [bDone, bSuccess, Code, ProblemCode]()
	{
		UPlayServSubsystem::Get()->GetData()->Get(TEXT("TestServerOnlyEntity"), ErrorMissingRecId,
			FPlayServJsonCallback::CreateLambda(
				[bDone, bSuccess, Code, ProblemCode](bool bOk, const TSharedPtr<FJsonObject>&, const FPlayServError& Error)
		{
			*bSuccess = bOk;
			*Code = Error.Code;
			*ProblemCode = Error.ProblemCode;
			*bDone = true;
		}));
	}, bDone, 8.0f));

	AddCommand(new FPlayServAssertStep(this, [bSuccess, Code, ProblemCode](FAutomationTestBase* T)
	{
		T->TestFalse(TEXT("Client-plane read of a client-closed table fails"), *bSuccess);
		T->TestEqual(TEXT("ACL denial maps to Forbidden (403), not Unauthorized"), *Code, EPlayServErrorCode::Forbidden);
		T->TestEqual(TEXT("ProblemCode is table_read_forbidden"), *ProblemCode, FString(TEXT("table_read_forbidden")));
	}));

	return true;
}

// Subsystem Availability

// Verify the subsystem and all sub-modules are available in automation context.
// Null-check code paths exist in every PlayServ::Data:: namespace entry point
// (SubsystemUnavailable fallbacks) but cannot be triggered in UE automation since
// engine subsystems are always initialized.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServErrorSubsystemAvailableTest,
	"PlayServ.Data.ErrorHandling.SubsystemAvailable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServErrorSubsystemAvailableTest::RunTest(const FString& Parameters)
{
	UPlayServSubsystem* PS = UPlayServSubsystem::Get();
	TestNotNull(TEXT("UPlayServSubsystem::Get() returns valid pointer"), PS);

	if (PS)
	{
		TestNotNull(TEXT("GetAuth() returns valid pointer"), PS->GetAuth());
		TestNotNull(TEXT("GetData() returns valid pointer"), PS->GetData());
		TestNotNull(TEXT("GetCode() returns valid pointer"), PS->GetCode());
	}

	return true;
}

// ---------------------------------------------------------------------------
// Bug Fix Tests — M3 Correctness
// ---------------------------------------------------------------------------

// BulkSave on a batch of NEW entities (was SaveAllMixedValidity — "mixed id validity" is gone:
// every new entity is id-less until save on V2). Covers both construction routes (the Create<T>
// facade and a raw NewObject): BulkSave creates each row and binds a distinct server id.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServErrorBulkSaveBindsServerIdsTest,
	"PlayServ.Data.ErrorHandling.BulkSaveBindsServerIds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServErrorBulkSaveBindsServerIdsTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("error-bulksave-ids"));

	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	TSharedPtr<bool> bSaveSuccess = MakeShared<bool>(false);
	TSharedPtr<FString> ViaCreateId = MakeShared<FString>();
	TSharedPtr<FString> ViaNewObjectId = MakeShared<FString>();
	TSharedPtr<FString> ErrorMsg = MakeShared<FString>();

	AddCommand(new FPlayServAsyncStep(this, TEXT("BulkSaveNewEntities"),
		[bDone, bSaveSuccess, ViaCreateId, ViaNewObjectId, ErrorMsg]()
	{
		UPlayServSubsystem* PS = UPlayServSubsystem::Get();
		if (!PS) { *bDone = true; return; }

		// One entity via the Create facade, one via raw NewObject — both id-less until the
		// batch create lands and binds the server-minted ids.
		UTestPlayer* ViaCreate = PlayServ::Data::Create<UTestPlayer>();
		ViaCreate->Name = TEXT("ViaCreate");

		UTestPlayer* ViaNewObject = NewObject<UTestPlayer>(PS);
		ViaNewObject->Name = TEXT("ViaNewObject");

		TArray<UObject*> Batch = { ViaCreate, ViaNewObject };
		PlayServ::Data::BulkSave(Batch, FPlayServSimpleCallback::CreateLambda(
			[bDone, bSaveSuccess, ViaCreateId, ViaNewObjectId, ErrorMsg, ViaCreate, ViaNewObject]
			(bool bSuccess, const FPlayServError& Error)
		{
			*bSaveSuccess = bSuccess;
			*ViaCreateId = PlayServ::Data::GetRecordId(ViaCreate);
			*ViaNewObjectId = PlayServ::Data::GetRecordId(ViaNewObject);
			*ErrorMsg = Error.Message;
			*bDone = true;
		}));
	}, bDone, 5.0f));

	AddCommand(new FPlayServAssertStep(this, [bSaveSuccess, ViaCreateId, ViaNewObjectId, ErrorMsg](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("BulkSave succeeds on a batch of new entities"), *bSaveSuccess);
		T->TestTrue(TEXT("Create-facade entity got a server id"), ViaCreateId->StartsWith(TEXT("rec_")));
		T->TestTrue(TEXT("Raw NewObject entity got a server id"), ViaNewObjectId->StartsWith(TEXT("rec_")));
		T->TestNotEqual(TEXT("The two rows carry distinct ids"), *ViaCreateId, *ViaNewObjectId);
		T->AddInfo(FString::Printf(TEXT("ViaCreate: %s | ViaNewObject: %s"), **ViaCreateId, **ViaNewObjectId));
		if (!*bSaveSuccess)
		{
			T->AddError(FString::Printf(TEXT("Error: %s"), **ErrorMsg));
		}
	}));

	AddErrorCleanupStep(this);
	return true;
}

// Empty-ID delete rejection — PSDeleteSelf on a never-saved entity. The guard fires at op RUN
// time (the delete is enqueued first — V2 moved the id guards behind the per-entity queue so
// ops legally enqueued after a first Save see the id the create binds), and a never-saved
// entity still has no id when its turn comes.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServErrorEmptyIdDeleteTest,
	"PlayServ.Data.ErrorHandling.EmptyIdDelete",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServErrorEmptyIdDeleteTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("error-emptyid-delete"));

	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	TSharedPtr<bool> bDeleteSuccess = MakeShared<bool>(true);
	TSharedPtr<FString> ErrorMsg = MakeShared<FString>();

	AddCommand(new FPlayServAsyncStep(this, TEXT("DeleteNoId"),
		[bDone, bDeleteSuccess, ErrorMsg]()
	{
		UPlayServSubsystem* PS = UPlayServSubsystem::Get();
		if (!PS) { *bDone = true; return; }

		UTestPlayer* NoId = NewObject<UTestPlayer>(PS);
		PlayServ::Data::Delete(NoId, FPlayServSimpleCallback::CreateLambda(
			[bDone, bDeleteSuccess, ErrorMsg](bool bSuccess, const FPlayServError& Error)
		{
			*bDeleteSuccess = bSuccess;
			*ErrorMsg = Error.Message;
			*bDone = true;
		}));
	}, bDone, 5.0f));

	AddCommand(new FPlayServAssertStep(this, [bDeleteSuccess, ErrorMsg](FAutomationTestBase* T)
	{
		T->TestFalse(TEXT("PSDeleteSelf fails on entity without ID"), *bDeleteSuccess);
		T->TestTrue(TEXT("Error mentions no ID"), (*ErrorMsg).Contains(TEXT("no ID")));
	}));

	AddErrorCleanupStep(this);
	return true;
}

// Transient property filtering — verify UPROPERTY(Transient) at top level and inside USTRUCT
// is NOT serialized to backend.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServErrorTransientFilteringTest,
	"PlayServ.Data.ErrorHandling.TransientPropertyFiltering",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServErrorTransientFilteringTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("error-transient"));

	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	TSharedPtr<bool> bRoundtripOk = MakeShared<bool>(false);
	TSharedPtr<FString> LoadedLocalCache = MakeShared<FString>();
	TSharedPtr<FString> LoadedCachedDisplay = MakeShared<FString>();

	AddCommand(new FPlayServAsyncStep(this, TEXT("CreateSaveLoadVerify"),
		[bDone, bRoundtripOk, LoadedLocalCache, LoadedCachedDisplay]()
	{
		UPlayServSubsystem* PS = UPlayServSubsystem::Get();
		if (!PS) { *bDone = true; return; }

		// Create entity with transient fields set to non-default values
		UTestPlayer* Player = PlayServ::Data::Create<UTestPlayer>();
		Player->Name = TEXT("TransientTest");
		Player->Level = 42;
		Player->LocalCacheData = TEXT("ShouldNotPersist");
		Player->Profile.Bio = TEXT("TestBio");
		Player->Profile.CachedDisplayName = TEXT("ShouldNotPersistEither");

		TWeakObjectPtr<UPlayServSubsystem> WeakPS(PS);
		TWeakObjectPtr<UTestPlayer> WeakPlayer(Player);
		PlayServ::Data::Save(Player,FPlayServSimpleCallback::CreateLambda([WeakPS, WeakPlayer, bDone, bRoundtripOk, LoadedLocalCache, LoadedCachedDisplay](bool bSaveOk, const FPlayServError&)
		{
			if (!bSaveOk) { *bDone = true; return; }

			UPlayServSubsystem* PS = WeakPS.Get();
			UTestPlayer* Player = WeakPlayer.Get();
			if (!PS || !Player) { *bDone = true; return; }

			FString Id = UPlayServData::GetRecordId(Player);
			PlayServ::Data::Load<UTestPlayer>(Id,
				[bDone, bRoundtripOk, LoadedLocalCache, LoadedCachedDisplay](bool bLoadOk, UTestPlayer* Loaded, const FPlayServError&)
			{
				if (bLoadOk && Loaded)
				{
					*bRoundtripOk = true;
					*LoadedLocalCache = Loaded->LocalCacheData;
					*LoadedCachedDisplay = Loaded->Profile.CachedDisplayName;
				}
				*bDone = true;
			});
		}));
	}, bDone, 5.0f));

	AddCommand(new FPlayServAssertStep(this,
		[bRoundtripOk, LoadedLocalCache, LoadedCachedDisplay](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("Save+Load roundtrip succeeded"), *bRoundtripOk);
		T->TestTrue(TEXT("Top-level Transient field is empty after reload"),
			(*LoadedLocalCache).IsEmpty());
		T->TestTrue(TEXT("USTRUCT Transient field is empty after reload"),
			(*LoadedCachedDisplay).IsEmpty());
	}));

	AddErrorCleanupStep(this);
	return true;
}

// LoadAllEntities with malformed 2xx response — missing 'items' field.
// This tests the SDK-side guard, not the backend. We can't easily force the backend
// to return a malformed 2xx, so this is a structural verification that the guard exists
// by testing the normal path doesn't false-positive.
// The actual malformed-response test would require a mock HTTP layer.

// Subsystem accessor checkf() — verified by SubsystemAvailable test above.
// Cannot test the crash path in automation (checkf kills the process).

#endif // !UE_BUILD_SHIPPING
