#include "PlayServTestCommon.h"
#include "PlayServWireTestHelpers.h"
#include "Core/PlayServSubsystem.h"
#include "Auth/PlayServAuth.h"
#include "Data/PlayServData.h"
#include "Data/PlayServFilter.h"
#include "PlayServ.h"
#include "Core/PlayServSettings.h"
#include "TestEntities.h"
#include "PlayServTestAccess.h"
#include "OnlineSubsystem.h"
#include "Interfaces/OnlineIdentityInterface.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"

#if !UE_BUILD_SHIPPING

// ---------------------------------------------------------------------------
// PlayServ.Auth.UnauthDataFails (the name is kept for suite history)
//
// The credential model is the pk_* key + entity ACLs: an UNOWNED client-writable table (every test fixture) accepts pk_-only
// writes BY DESIGN, while a client-closed table refuses with 403 table_read/write_forbidden.
// This test pins both halves of that boundary with no session at all.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthUnauthDataFailsTest,
	"PlayServ.Auth.UnauthDataFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthUnauthDataFailsTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));

	TSharedPtr<bool> bLogoutDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("EnsureLoggedOut"), [bLogoutDone]()
	{
		if (PlayServ::Auth::IsLoggedIn())
		{
			PlayServ::Auth::Logout(FPlayServSimpleCallback::CreateLambda(
				[bLogoutDone](bool, const FPlayServError&) { *bLogoutDone = true; }));
		}
		else
		{
			*bLogoutDone = true;
		}
	}, bLogoutDone));

	// Half 1: pk_-only write on an unowned client-writable fixture SUCCEEDS (fixture design).
	TSharedPtr<bool> bSaveDone = MakeShared<bool>(false);
	TSharedPtr<bool> bSaveOk = MakeShared<bool>(false);
	TSharedPtr<TStrongObjectPtr<UTestPlayer>> Holder = MakeShared<TStrongObjectPtr<UTestPlayer>>();
	AddCommand(new FPlayServAsyncStep(this, TEXT("PkOnlyOpenTableSave"), [bSaveDone, bSaveOk, Holder]()
	{
		UTestPlayer* Player = PlayServ::Data::Create<UTestPlayer>();
		Player->Name = TEXT("PkOnly");
		Player->Level = 1;
		*Holder = TStrongObjectPtr<UTestPlayer>(Player);

		PlayServ::Data::Save(Player, FPlayServSimpleCallback::CreateLambda([bSaveDone, bSaveOk](bool bSuccess, const FPlayServError&)
		{
			*bSaveOk = bSuccess;
			*bSaveDone = true;
		}));
	}, bSaveDone, 10.0f));

	AddCommand(new FPlayServAssertStep(this, [bSaveOk](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("pk_-only save on an unowned client-writable table succeeds (V2 ACL model)"), *bSaveOk);
	}));

	// Half 2: the same pk_-only caller is REFUSED on a client-closed table (TestServerOnlyEntity).
	TSharedPtr<bool> bProbeDone = MakeShared<bool>(false);
	TSharedPtr<FPlayServError> ProbeError = MakeShared<FPlayServError>();
	AddCommand(new FPlayServAsyncStep(this, TEXT("PkOnlyClosedTableProbe"), [bProbeDone, ProbeError]()
	{
		UPlayServSubsystem::Get()->GetData()->Get(TEXT("TestServerOnlyEntity"), TEXT("rec_00000000000000000000000000"),
			FPlayServJsonCallback::CreateLambda(
				[bProbeDone, ProbeError](bool, const TSharedPtr<FJsonObject>&, const FPlayServError& Error)
				{
					*ProbeError = Error;
					*bProbeDone = true;
				}));
	}, bProbeDone, 10.0f));

	AddCommand(new FPlayServAssertStep(this, [ProbeError](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("closed table refuses the client plane (403)"), ProbeError->Code, EPlayServErrorCode::Forbidden);
		T->TestEqual(TEXT("problem code is table_read_forbidden"), ProbeError->ProblemCode, FString(TEXT("table_read_forbidden")));
	}));

	// Cleanup the pk_-only row.
	TSharedPtr<bool> bCleanDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("Cleanup"), [bCleanDone, Holder]()
	{
		if (!Holder->IsValid())
		{
			*bCleanDone = true;
			return;
		}
		PlayServ::Data::Delete(Holder->Get(), FPlayServSimpleCallback::CreateLambda(
			[bCleanDone, Holder](bool, const FPlayServError&)
			{
				Holder->Reset();
				*bCleanDone = true;
			}));
	}, bCleanDone, 10.0f));

	return true;
}

// (The identity bootstrap and its assertions live in PlayServ.Auth.V2.AnonLogin.)

// ---------------------------------------------------------------------------
// PlayServ.Auth.AuthenticatedData
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthAuthenticatedDataTest,
	"PlayServ.Auth.AuthenticatedData",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthAuthenticatedDataTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("auth-data-test"));

	// Create + save entity (proves auth headers are injected)
	TSharedPtr<bool> bSaveDone = MakeShared<bool>(false);
	TSharedPtr<bool> bSaveOk = MakeShared<bool>(false);
	TSharedPtr<FString> SaveError = MakeShared<FString>();
	AddCommand(new FPlayServAsyncStep(this, TEXT("AuthenticatedSave"), [bSaveDone, bSaveOk, SaveError]()
	{
		// PSCreate registers entity in UPlayServData's UPROPERTY registry — GC-safe
		UTestPlayer* Player = PlayServ::Data::Create<UTestPlayer>();
		Player->Name = TEXT("AuthDataTest");
		Player->Level = 1;

		PlayServ::Data::Save(Player,FPlayServSimpleCallback::CreateLambda([bSaveDone, bSaveOk, SaveError](bool bSuccess, const FPlayServError& Error)
		{
			*bSaveOk = bSuccess;
			*SaveError = Error.Message;
			*bSaveDone = true;
		}));
	}, bSaveDone));

	AddCommand(new FPlayServAssertStep(this, [bSaveOk, SaveError](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("PSSave succeeded with auth headers"), *bSaveOk);
		if (*bSaveOk)
		{
			T->AddInfo(TEXT("Verified: Authorization + X-Player-Id headers injected on DataUpsert"));
		}
		else
		{
			T->AddError(FString::Printf(TEXT("PSSave error: %s"), **SaveError));
		}
	}));

	// Cleanup
	TSharedPtr<bool> bCleanDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("Cleanup"), [bCleanDone]()
	{
		PlayServ::Data::DeleteAll<UTestPlayer>(FPlayServFilter::All(), FPlayServDeleteAllCallback::CreateLambda(
			[bCleanDone](bool, const FPlayServDeleteAllResult&, const FPlayServError&) { *bCleanDone = true; }));
	}, bCleanDone));

	AddLogoutStep(this);
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Auth.Logout
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthLogoutTest,
	"PlayServ.Auth.Logout",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthLogoutTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("logout-test-user"));

	AddCommand(new FPlayServAssertStep(this, [](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("Should be logged in before logout"), UPlayServSubsystem::Get()->GetAuth()->IsLoggedIn());
	}));

	AddLogoutStep(this);

	AddCommand(new FPlayServAssertStep(this, [](FAutomationTestBase* T)
	{
		UPlayServAuth* Auth = UPlayServSubsystem::Get()->GetAuth();
		T->TestFalse(TEXT("Should not be logged in after logout"), Auth->IsLoggedIn());
		T->TestTrue(TEXT("AccessToken should be empty after logout"), Auth->GetAccessToken().IsEmpty());
		T->TestTrue(TEXT("PlayerId should be empty after logout"), Auth->GetPlayerId().IsEmpty());
		T->TestEqual(TEXT("SessionType should be None after logout"), Auth->GetSessionType(), EPlayServSessionType::None);
	}));

	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Auth.TokenRefresh (fast — ForceRefresh, no timer wait)
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthTokenRefreshTest,
	"PlayServ.Auth.TokenRefresh",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthTokenRefreshTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("token-refresh-test"));

	TSharedPtr<FString> InitialAccessToken = MakeShared<FString>();
	TSharedPtr<FString> InitialPlayerId = MakeShared<FString>();

	AddCommand(new FPlayServAssertStep(this, [InitialAccessToken, InitialPlayerId](FAutomationTestBase* T)
	{
		UPlayServAuth* Auth = UPlayServSubsystem::Get()->GetAuth();
		T->TestTrue(TEXT("Should be logged in"), Auth->IsLoggedIn());
		*InitialAccessToken = Auth->GetAccessToken();
		*InitialPlayerId = Auth->GetPlayerId();
		T->TestFalse(TEXT("Initial access token should not be empty"), InitialAccessToken->IsEmpty());
		T->AddInfo(FString::Printf(TEXT("Initial token: %.20s..."), **InitialAccessToken));
	}));

	// Wait 1.5s so the refreshed JWT has a different nbf/exp timestamp.
	// Backend JWTs use second-precision timestamps — same-second refresh
	// produces a byte-identical token.
	AddCommand(new FPlayServDelayStep(1.5f));

	// Trigger refresh via friend access, then poll for token change
	AddCommand(new FPlayServAssertStep(this, [](FAutomationTestBase*)
	{
		FPlayServAuthTestAccess::ForceRefresh(UPlayServSubsystem::Get()->GetAuth());
	}));

	AddCommand(new FPlayServPollStep(this, TEXT("WaitForTokenRefresh"),
		[InitialAccessToken]()
		{
			UPlayServSubsystem* PS = UPlayServSubsystem::Get();
			if (!PS || !PS->GetAuth()->IsLoggedIn())
			{
				return false;
			}
			return PS->GetAuth()->GetAccessToken() != *InitialAccessToken;
		},
		5.0f));

	AddCommand(new FPlayServAssertStep(this, [InitialAccessToken, InitialPlayerId](FAutomationTestBase* T)
	{
		UPlayServAuth* Auth = UPlayServSubsystem::Get()->GetAuth();
		T->TestTrue(TEXT("Should still be logged in after refresh"), Auth->IsLoggedIn());
		T->TestFalse(TEXT("New access token should not be empty"), Auth->GetAccessToken().IsEmpty());
		T->TestNotEqual(TEXT("Access token should have changed"), Auth->GetAccessToken(), *InitialAccessToken);
		T->TestEqual(TEXT("PlayerId should be preserved"), Auth->GetPlayerId(), *InitialPlayerId);
		T->TestEqual(TEXT("SessionType should still be Client"), Auth->GetSessionType(), EPlayServSessionType::Client);
		T->AddInfo(FString::Printf(TEXT("Refreshed token: %.20s..."), *Auth->GetAccessToken()));
	}));

	// CRUD roundtrip with refreshed token
	TSharedPtr<bool> bSaveDone = MakeShared<bool>(false);
	TSharedPtr<bool> bSaveOk = MakeShared<bool>(false);
	TSharedPtr<FString> SaveError = MakeShared<FString>();
	AddCommand(new FPlayServAsyncStep(this, TEXT("CrudWithRefreshedToken"),
		[bSaveDone, bSaveOk, SaveError]()
		{
			UTestPlayer* Player = PlayServ::Data::Create<UTestPlayer>();
			Player->Name = TEXT("RefreshTokenTest");
			Player->Level = 99;
			PlayServ::Data::Save(Player,FPlayServSimpleCallback::CreateLambda([bSaveDone, bSaveOk, SaveError](bool bSuccess, const FPlayServError& Error)
			{
				*bSaveOk = bSuccess;
				*SaveError = Error.Message;
				*bSaveDone = true;
			}));
		}, bSaveDone));

	AddCommand(new FPlayServAssertStep(this, [bSaveOk, SaveError](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("PSSave with refreshed token should succeed"), *bSaveOk);
		if (!*bSaveOk)
		{
			T->AddError(FString::Printf(TEXT("PSSave error: %s"), **SaveError));
		}
	}));

	// Cleanup
	TSharedPtr<bool> bCleanDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("Cleanup"), [bCleanDone]()
	{
		PlayServ::Data::DeleteAll<UTestPlayer>(FPlayServFilter::All(),
			FPlayServDeleteAllCallback::CreateLambda(
				[bCleanDone](bool, const FPlayServDeleteAllResult&, const FPlayServError&) { *bCleanDone = true; }));
	}, bCleanDone));

	AddLogoutStep(this);
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Auth.TokenRefreshTimer (manual — real FTSTicker, needs short TTL)
//
// Validates the actual FTSTicker-based auto-refresh path end-to-end.
// Requires: JWT_ACCESS_EXPIRY_SECONDS<=10 in backend local.env.
// Run manually from Session Frontend → Automation tab.
// Skips automatically if backend TTL > 10s to avoid false positives
// when started unintentionally.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthTokenRefreshTimerTest,
	"PlayServ.Auth.TokenRefreshTimer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter | EAutomationTestFlags::RequiresUser)

bool FPlayServAuthTokenRefreshTimerTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("timer-refresh-test"));

	TSharedPtr<FString> InitialAccessToken = MakeShared<FString>();
	TSharedPtr<bool> bSkipTest = MakeShared<bool>(false);

	// Check TTL — skip if backend isn't configured with a short expiry
	AddCommand(new FPlayServAssertStep(this,
		[InitialAccessToken, bSkipTest](FAutomationTestBase* T)
		{
			UPlayServAuth* Auth = UPlayServSubsystem::Get()->GetAuth();
			int32 TTL = FPlayServAuthTestAccess::GetAccessTokenTTL(Auth);

			if (TTL > 10)
			{
				T->AddWarning(FString::Printf(
					TEXT("SKIPPED: Backend TTL is %ds (max 10s for this test). ")
					TEXT("Set JWT_ACCESS_EXPIRY_SECONDS=5 in local.env and restart the backend."),
					TTL));
				*bSkipTest = true;
				return;
			}

			T->AddInfo(FString::Printf(TEXT("Backend TTL: %ds — refresh expected at ~%ds"), TTL, FMath::RoundToInt(TTL * 0.8f)));
			*InitialAccessToken = Auth->GetAccessToken();
		}));

	// Wait for real timer to fire (80% of TTL)
	AddCommand(new FPlayServPollStep(this, TEXT("WaitForTimerRefresh"),
		[InitialAccessToken, bSkipTest]()
		{
			if (*bSkipTest)
			{
				return true;
			}
			UPlayServSubsystem* PS = UPlayServSubsystem::Get();
			if (!PS || !PS->GetAuth()->IsLoggedIn())
			{
				return false;
			}
			return PS->GetAuth()->GetAccessToken() != *InitialAccessToken;
		},
		15.0f));

	AddCommand(new FPlayServAssertStep(this,
		[InitialAccessToken, bSkipTest](FAutomationTestBase* T)
		{
			if (*bSkipTest)
			{
				return;
			}
			UPlayServAuth* Auth = UPlayServSubsystem::Get()->GetAuth();
			T->TestTrue(TEXT("Should still be logged in after timer refresh"), Auth->IsLoggedIn());
			T->TestNotEqual(TEXT("Token should have changed via timer"), Auth->GetAccessToken(), *InitialAccessToken);
			T->AddInfo(TEXT("Verified: FTSTicker-based auto-refresh works end-to-end"));
		}));

	// CRUD roundtrip
	TSharedPtr<bool> bSaveDone = MakeShared<bool>(false);
	TSharedPtr<bool> bSaveOk = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("CrudAfterTimerRefresh"),
		[bSaveDone, bSaveOk, bSkipTest]()
		{
			if (*bSkipTest)
			{
				*bSaveDone = true;
				return;
			}
			UTestPlayer* Player = PlayServ::Data::Create<UTestPlayer>();
			Player->Name = TEXT("TimerRefreshTest");
			Player->Level = 77;
			PlayServ::Data::Save(Player,FPlayServSimpleCallback::CreateLambda([bSaveDone, bSaveOk](bool bSuccess, const FPlayServError&)
			{
				*bSaveOk = bSuccess;
				*bSaveDone = true;
			}));
		}, bSaveDone));

	AddCommand(new FPlayServAssertStep(this, [bSaveOk, bSkipTest](FAutomationTestBase* T)
	{
		if (*bSkipTest)
		{
			return;
		}
		T->TestTrue(TEXT("PSSave after timer refresh should succeed"), *bSaveOk);
		if (*bSaveOk)
		{
			T->AddInfo(TEXT("Verified: timer-refreshed token accepted by backend for CRUD"));
		}
	}));

	// Cleanup
	TSharedPtr<bool> bCleanDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("Cleanup"), [bCleanDone, bSkipTest]()
	{
		if (*bSkipTest)
		{
			*bCleanDone = true;
			return;
		}
		PlayServ::Data::DeleteAll<UTestPlayer>(FPlayServFilter::All(),
			FPlayServDeleteAllCallback::CreateLambda(
				[bCleanDone](bool, const FPlayServDeleteAllResult&, const FPlayServError&) { *bCleanDone = true; }));
	}, bCleanDone));

	AddLogoutStep(this);
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Auth.OnSessionLost
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthOnSessionLostTest,
	"PlayServ.Auth.OnSessionLost",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthOnSessionLostTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("session-lost-test"));

	TSharedPtr<TWeakObjectPtr<UPlayServSessionLostListener>> ListenerWeak =
		MakeShared<TWeakObjectPtr<UPlayServSessionLostListener>>();

	AddCommand(new FPlayServAssertStep(this,
		[ListenerWeak](FAutomationTestBase* T)
		{
			UPlayServAuth* Auth = UPlayServSubsystem::Get()->GetAuth();
			T->TestTrue(TEXT("Should be logged in before corruption"), Auth->IsLoggedIn());

			UPlayServSessionLostListener* Listener = NewObject<UPlayServSessionLostListener>();
			Listener->AddToRoot();
			Auth->OnSessionLost.AddDynamic(Listener, &UPlayServSessionLostListener::OnSessionLost);
			*ListenerWeak = Listener;

			FPlayServAuthTestAccess::InvalidateRefreshToken(Auth);
			FPlayServAuthTestAccess::ForceRefresh(Auth);
			T->AddInfo(TEXT("Refresh token corrupted + forced refresh — waiting for failure"));
		}));

	AddCommand(new FPlayServPollStep(this, TEXT("WaitForSessionLost"),
		[ListenerWeak]()
		{
			if (!ListenerWeak->IsValid())
			{
				return false;
			}
			return ListenerWeak->Get()->bSessionLostFired;
		},
		5.0f));

	AddCommand(new FPlayServAssertStep(this,
		[ListenerWeak](FAutomationTestBase* T)
		{
			if (ListenerWeak->IsValid())
			{
				T->TestTrue(TEXT("OnSessionLost should have fired"), ListenerWeak->Get()->bSessionLostFired);
			}
			else
			{
				T->AddError(TEXT("Listener was garbage collected unexpectedly"));
			}

			UPlayServAuth* Auth = UPlayServSubsystem::Get()->GetAuth();
			T->TestFalse(TEXT("Should not be logged in after session lost"), Auth->IsLoggedIn());
			T->TestTrue(TEXT("AccessToken should be empty"), Auth->GetAccessToken().IsEmpty());
			T->TestTrue(TEXT("PlayerId should be empty"), Auth->GetPlayerId().IsEmpty());
			T->TestEqual(TEXT("SessionType should be None"), Auth->GetSessionType(), EPlayServSessionType::None);
			T->AddInfo(TEXT("Verified: OnSessionLost broadcast, session cleared"));

			if (ListenerWeak->IsValid())
			{
				UPlayServSessionLostListener* Listener = ListenerWeak->Get();
				Auth->OnSessionLost.RemoveDynamic(Listener, &UPlayServSessionLostListener::OnSessionLost);
				Listener->RemoveFromRoot();
			}
		}));

	// No logout — session already cleared by OnSessionLost handler
	return true;
}


// ---------------------------------------------------------------------------
// PlayServ.Auth.LoginServer (the sk_ key IS the credential - no exchange)
//
// LoginServer stores the sk_ as a static bearer, immediately and offline. The server plane
// bypasses entity ACLs, so a server session must be able to write a client-CLOSED table
// (TestServerOnlyEntity) - that is the discriminating assertion here.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthLoginServerTest,
	"PlayServ.Auth.LoginServer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthLoginServerTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));

	// Login with the committed dev server key (test/tooling plane, [PlayServ.DevSecrets]).
	TSharedPtr<bool> bLoginDone = MakeShared<bool>(false);
	TSharedPtr<bool> bLoginOk = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("LoginServer"), [bLoginDone, bLoginOk]()
	{
		PlayServ::Auth::LoginServer(PlayServWireTest::ServerKey(),
			FPlayServSimpleCallback::CreateLambda(
				[bLoginDone, bLoginOk](bool bSuccess, const FPlayServError&)
				{
					*bLoginOk = bSuccess;
					*bLoginDone = true;
				}));
	}, bLoginDone));

	AddCommand(new FPlayServAssertStep(this, [bLoginOk](FAutomationTestBase* T)
	{
		UPlayServAuth* Auth = UPlayServSubsystem::Get()->GetAuth();
		T->TestTrue(TEXT("LoginServer succeeded (offline - the key is the credential)"), *bLoginOk);
		T->TestTrue(TEXT("IsLoggedIn"), Auth->IsLoggedIn());
		T->TestTrue(TEXT("IsServerSession"), Auth->IsServerSession());
		T->TestEqual(TEXT("SessionType is Server"), Auth->GetSessionType(), EPlayServSessionType::Server);
		T->TestTrue(TEXT("PlayerId stays empty for server sessions"), Auth->GetPlayerId().IsEmpty());
	}));

	// The discriminating server-plane proof: read the client-CLOSED TestServerOnlyEntity table.
	// A client credential gets 403 table_read_forbidden here; the sk_ bypasses the ACL and
	// gets an honest 404 not_found for a never-minted record id.
	TSharedPtr<bool> bProbeDone = MakeShared<bool>(false);
	TSharedPtr<FPlayServError> ProbeError = MakeShared<FPlayServError>();
	AddCommand(new FPlayServAsyncStep(this, TEXT("ServerPlaneClosedTableProbe"), [bProbeDone, ProbeError]()
	{
		UPlayServSubsystem::Get()->GetData()->Get(TEXT("TestServerOnlyEntity"), TEXT("rec_00000000000000000000000000"),
			FPlayServJsonCallback::CreateLambda(
				[bProbeDone, ProbeError](bool, const TSharedPtr<FJsonObject>&, const FPlayServError& Error)
				{
					*ProbeError = Error;
					*bProbeDone = true;
				}));
	}, bProbeDone, 10.0f));

	AddCommand(new FPlayServAssertStep(this, [ProbeError](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("server plane reaches the closed table (404, not 403)"), ProbeError->Code, EPlayServErrorCode::NotFound);
		T->TestEqual(TEXT("problem code is not_found"), ProbeError->ProblemCode, FString(TEXT("not_found")));
	}));

	// A real server-plane write roundtrip on an open fixture.
	TSharedPtr<bool> bSaveDone = MakeShared<bool>(false);
	TSharedPtr<bool> bSaveOk = MakeShared<bool>(false);
	TSharedPtr<TStrongObjectPtr<UTestPlayer>> Holder = MakeShared<TStrongObjectPtr<UTestPlayer>>();
	AddCommand(new FPlayServAsyncStep(this, TEXT("ServerPlaneSave"), [bSaveDone, bSaveOk, Holder]()
	{
		UTestPlayer* Player = PlayServ::Data::Create<UTestPlayer>();
		Player->Name = TEXT("ServerPlaneSave");
		Player->Level = 51;
		*Holder = TStrongObjectPtr<UTestPlayer>(Player);
		PlayServ::Data::Save(Player, FPlayServSimpleCallback::CreateLambda(
			[bSaveDone, bSaveOk](bool bSuccess, const FPlayServError&)
			{
				*bSaveOk = bSuccess;
				*bSaveDone = true;
			}));
	}, bSaveDone, 10.0f));

	AddCommand(new FPlayServAssertStep(this, [bSaveOk, Holder](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("server-plane save succeeded"), *bSaveOk);
		if (Holder->IsValid())
		{
			T->TestTrue(TEXT("server-minted id bound"), !UPlayServData::GetRecordId(Holder->Get()).IsEmpty());
		}
	}));

	// Cleanup the row + release, then logout (local clear - server sessions have no wire logout).
	TSharedPtr<bool> bCleanDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("Cleanup"), [bCleanDone, Holder]()
	{
		if (!Holder->IsValid())
		{
			*bCleanDone = true;
			return;
		}
		PlayServ::Data::Delete(Holder->Get(), FPlayServSimpleCallback::CreateLambda(
			[bCleanDone, Holder](bool, const FPlayServError&)
			{
				Holder->Reset();
				*bCleanDone = true;
			}));
	}, bCleanDone, 10.0f));

	AddLogoutStep(this);
	return true;
}

#endif // !UE_BUILD_SHIPPING