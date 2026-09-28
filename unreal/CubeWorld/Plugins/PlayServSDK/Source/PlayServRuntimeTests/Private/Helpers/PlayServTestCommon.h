#pragma once

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "Core/PlayServSubsystem.h"
#include "Auth/PlayServAuth.h"
#include "PlayServ.h"

/**
 * Generic latent command for any async PlayServ operation.
 *
 * Fires the Action lambda on first Update(), then polls bDone until the callback
 * sets it to true. Completes instantly when the callback fires (~50-200ms).
 * MaxTimeout is a safety net to prevent infinite hangs.
 */
class FPlayServAsyncStep : public IAutomationLatentCommand
{
public:
	FPlayServAsyncStep(FAutomationTestBase* InTest, const FString& InStepName, TFunction<void()> InAction, TSharedPtr<bool> InDone, float InMaxTimeout = 1.0f)
		: Test(InTest)
		, StepName(InStepName)
		, Action(MoveTemp(InAction))
		, bDone(InDone)
		, MaxTimeout(InMaxTimeout)
		, bStarted(false)
	{
	}

	// Test pointer lifetime: IAutomationLatentCommand is owned by the test runner
	// and destroyed before the FAutomationTestBase instance.
	bool Update() override
	{
		if (!bStarted)
		{
			Action();
			bStarted = true;
		}

		if (*bDone)
		{
			return true;
		}

		if (GetCurrentRunTime() > MaxTimeout)
		{
			Test->AddError(FString::Printf(TEXT("[%s] Timed out after %.1fs"), *StepName, MaxTimeout));
			return true;
		}

		return false;
	}

private:
	FAutomationTestBase* Test;
	FString StepName;
	TFunction<void()> Action;
	TSharedPtr<bool> bDone;
	float MaxTimeout;
	bool bStarted;
};

/**
 * Verify PlayServ subsystem is available before running tests.
 * Settings are configured via command-line args (-PlayServBaseURL=, -PlayServGameId=).
 */
class FPlayServSetupStep : public IAutomationLatentCommand
{
public:
	FPlayServSetupStep(FAutomationTestBase* InTest)
		: Test(InTest)
	{
	}

	bool Update() override
	{
		UPlayServSubsystem* PS = UPlayServSubsystem::Get();
		if (!PS)
		{
			Test->AddError(TEXT("UPlayServSubsystem not available. Engine not initialized?"));
			return true;
		}

		return true;
	}

private:
	FAutomationTestBase* Test;
};

/**
 * Synchronous assertion step — runs assertions and completes immediately.
 * Captures the Test pointer so callers don't need to manually alias `this`.
 */
class FPlayServAssertStep : public IAutomationLatentCommand
{
public:
	FPlayServAssertStep(FAutomationTestBase* InTest, TFunction<void(FAutomationTestBase*)> InAssertions)
		: Test(InTest)
		, Assertions(MoveTemp(InAssertions))
	{
	}

	bool Update() override
	{
		Assertions(Test);
		return true;
	}

private:
	FAutomationTestBase* Test;
	TFunction<void(FAutomationTestBase*)> Assertions;
};

/**
 * Poll a condition until it becomes true, or timeout.
 *
 * Unlike FPlayServAsyncStep which fires an action then waits for a callback,
 * this simply checks a condition lambda each tick — useful for observing
 * state changes driven by internal timers (e.g., token refresh).
 */
class FPlayServPollStep : public IAutomationLatentCommand
{
public:
	FPlayServPollStep(FAutomationTestBase* InTest, const FString& InStepName,
		TFunction<bool()> InCondition, float InMaxTimeout = 10.0f)
		: Test(InTest)
		, StepName(InStepName)
		, Condition(MoveTemp(InCondition))
		, MaxTimeout(InMaxTimeout)
	{
	}

	bool Update() override
	{
		if (Condition())
		{
			return true;
		}

		if (GetCurrentRunTime() > MaxTimeout)
		{
			Test->AddError(FString::Printf(TEXT("[%s] Condition not met after %.1fs"), *StepName, MaxTimeout));
			return true;
		}

		return false;
	}

private:
	FAutomationTestBase* Test;
	FString StepName;
	TFunction<bool()> Condition;
	float MaxTimeout;
};

/**
 * Wait for a fixed duration before proceeding.
 * Used when subsequent steps need a timing gap (e.g., JWT timestamp drift).
 */
class FPlayServDelayStep : public IAutomationLatentCommand
{
public:
	FPlayServDelayStep(float InDelay)
		: Delay(InDelay)
	{
	}

	bool Update() override
	{
		return GetCurrentRunTime() >= Delay;
	}

private:
	float Delay;
};

// ---------------------------------------------------------------------------
// Shared test helpers — login, logout, cleanup
// ---------------------------------------------------------------------------

// Identity bootstrap: anonymous login (every run mints a fresh plr_*, so tests must never
// assume a stable identity across runs). The Identifier parameter is a LABEL only, kept for
// log readability at the ~30 call sites; it does not select an identity.
/**
 * Prefix on every player this suite creates, so all of them are findable with one query.
 *
 * `list_players?q=` is a regex `contains` over the player's name (MongoPlayerAccountStore.ListAsync),
 * so this prefix is what lets an operator select the suite's leftovers as a group. It stays ASCII
 * and stays at the FRONT: the SDK caps a display name at 64 characters, and a long test identifier
 * is truncated from the tail, which leaves the prefix — and therefore the query — intact.
 */
inline const TCHAR* const SuiteDisplayNamePrefix = TEXT("UE SDK Automation");

/** "UE SDK Automation - <Identifier>", the display name for a player this suite mints. */
inline FString MakeSuiteDisplayName(const FString& Identifier)
{
	return Identifier.IsEmpty()
		? FString(SuiteDisplayNamePrefix)
		: FString::Printf(TEXT("%s - %s"), SuiteDisplayNamePrefix, *Identifier);
}

/**
 * Ensure a client session exists — REUSING a live one rather than minting another player.
 *
 * Every call used to be a LoginAnonymous, which mints a brand-new `plr_*`, so a full suite run left
 * 82 permanent guests on dev and nothing on the platform reaps them (PSV-2659, PSV-2660). That was
 * V1 -> V2 fallout: LoginDebug selected a player by identifier, so re-running the suite reused the
 * same handful, and the identifier here is the vestige of it — used only to label the step.
 *
 * Reuse is safe because the tests that call this are identity-agnostic: they need A session, and
 * none of them asserts WHICH player it is. The tests that do care — the Auth ones, which assert
 * fresh-mint behaviour, or revoke their token family — keep their own logins and are not routed
 * through here. The one test that needs NO session (PlayServ.Auth.UnauthDataFails) already forces
 * the issue with its own explicit logout rather than relying on the previous test.
 *
 * A SERVER session is deliberately not reused: it has no player, so a data test inheriting one
 * would silently exercise the server plane instead of the client plane it means to test.
 */
inline void AddLoginStep(FAutomationTestBase* Test, const FString& Identifier)
{
	// 8s timeout accommodates first-request backend warmup; steady-state is ~100-400ms.
	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	Test->AddCommand(new FPlayServAsyncStep(Test, FString::Printf(TEXT("EnsureSession[%s]"), *Identifier), [bDone, Identifier]()
	{
		if (PlayServ::Auth::IsLoggedIn() && !PlayServ::Auth::IsServerSession())
		{
			*bDone = true;
			return;
		}

		// Named, so the guests the suite does still create are identifiable in the operator console
		// instead of being 82 blank rows (PSV-2646 made the name reachable).
		PlayServ::Auth::LoginAnonymous(MakeSuiteDisplayName(Identifier),
			FPlayServAuthCallback::CreateLambda(
				[bDone](bool, const FString&, const FPlayServError&) { *bDone = true; }));
	}, bDone, 8.0f));
}

inline void AddLogoutStep(FAutomationTestBase* Test)
{
	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	Test->AddCommand(new FPlayServAsyncStep(Test, TEXT("Logout"), [bDone]()
	{
		PlayServ::Auth::Logout(FPlayServSimpleCallback::CreateLambda(
			[bDone](bool, const FPlayServError&) { *bDone = true; }));
	}, bDone));
}
