#include "PlayServTestCommon.h"
#include "Containers/StringConv.h"
#include "Misc/Base64.h"
#include "Misc/Guid.h"
#include "PlayServTestAccess.h"
#include "PlayServWireTestHelpers.h"
#include "Core/PlayServSubsystem.h"
#include "Core/PlayServHttp.h"
#include "PlayServ.h"

#if !UE_BUILD_SHIPPING

// ---------------------------------------------------------------------------
// PlayServ.Auth.V2.* — session lifecycle, live against the platform.
//
// Anonymous login is the identity bootstrap (every run mints fresh plr_* players; tests
// must not assume a stable identity).
// Refresh rotates the refresh token (reuse of a rotated token revokes the session family)
// and sign-out revokes server-side — both pinned here with live assertions.
// ---------------------------------------------------------------------------

namespace
{
	UPlayServAuth* AuthModule()
	{
		UPlayServSubsystem* PS = UPlayServSubsystem::Get();
		return PS ? PS->GetAuth() : nullptr;
	}

	/**
	 * A FRESH guest, every time — deliberately not AddLoginStep's reuse.
	 *
	 * These tests are about identity itself: two of them assert fresh-mint behaviour and two revoke
	 * the whole token family, so a shared session would either make them untestable or destroy it
	 * for everything after. They are the ~13 players a run legitimately creates (PSV-2659); the
	 * name is what makes those findable rather than blank.
	 */
	void AddAnonLoginStep(FAutomationTestBase* Test, TSharedPtr<FString> OutPlayerId)
	{
		TSharedPtr<bool> bDone = MakeShared<bool>(false);
		Test->AddCommand(new FPlayServAsyncStep(Test, TEXT("AnonLogin"), [bDone, OutPlayerId]()
		{
			PlayServ::Auth::LoginAnonymous(MakeSuiteDisplayName(TEXT("Auth.V2")), FPlayServAuthCallback::CreateLambda(
				[bDone, OutPlayerId](bool bSuccess, const FString& PlayerId, const FPlayServError&)
				{
					if (bSuccess)
					{
						*OutPlayerId = PlayerId;
					}
					*bDone = true;
				}));
		}, bDone, 8.0f));
	}

	void AddV2LogoutStep(FAutomationTestBase* Test)
	{
		TSharedPtr<bool> bDone = MakeShared<bool>(false);
		Test->AddCommand(new FPlayServAsyncStep(Test, TEXT("Logout"), [bDone]()
		{
			PlayServ::Auth::Logout(FPlayServSimpleCallback::CreateLambda(
				[bDone](bool, const FPlayServError&) { *bDone = true; }));
		}, bDone, 8.0f));
	}
}

// ---------------------------------------------------------------------------
// AnonLogin — mints a plr_* client session with wire-sourced token lifetimes.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthV2AnonLoginTest,
	"PlayServ.Auth.V2.AnonLogin",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthV2AnonLoginTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FString> PlayerId = MakeShared<FString>();
	AddAnonLoginStep(this, PlayerId);

	AddCommand(new FPlayServAssertStep(this, [PlayerId](FAutomationTestBase* T)
	{
		UPlayServAuth* Auth = AuthModule();
		if (Auth == nullptr)
		{
			T->AddError(TEXT("Auth module unavailable"));
			return;
		}
		T->TestTrue(TEXT("login succeeded (player id set)"), !PlayerId->IsEmpty());
		T->TestTrue(TEXT("player id is plr_*"), PlayerId->StartsWith(TEXT("plr_")));
		T->TestTrue(TEXT("IsLoggedIn"), Auth->IsLoggedIn());
		T->TestEqual(TEXT("session type is Client"), Auth->GetSessionType(), EPlayServSessionType::Client);
		T->TestEqual(TEXT("GetPlayerId matches the callback"), Auth->GetPlayerId(), *PlayerId);
		T->TestTrue(TEXT("access token stored"), !Auth->GetAccessToken().IsEmpty());
		T->TestTrue(TEXT("access TTL from the wire (expires_in)"), FPlayServAuthTestAccess::GetAccessTokenTTL(Auth) > 0);
		T->TestTrue(TEXT("refresh lifetime from the wire (refresh_expires_in)"),
			FPlayServAuthTestAccess::GetRefreshTokenLifetime(Auth) > 0);
	}));

	AddV2LogoutStep(this);
	return true;
}

// ---------------------------------------------------------------------------
// RefreshRotation — a forced refresh rotates BOTH tokens (access + refresh), keeps the
// session and the player identity, and single-flights.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthV2RefreshRotationTest,
	"PlayServ.Auth.V2.RefreshRotation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthV2RefreshRotationTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FString> PlayerId = MakeShared<FString>();
	TSharedPtr<FString> OldAccess = MakeShared<FString>();
	TSharedPtr<FString> OldRefresh = MakeShared<FString>();

	AddAnonLoginStep(this, PlayerId);

	TSharedPtr<bool> bCaptured = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("CaptureTokens"), [bCaptured, OldAccess, OldRefresh]()
	{
		if (UPlayServAuth* Auth = AuthModule())
		{
			*OldAccess = Auth->GetAccessToken();
			*OldRefresh = FPlayServAuthTestAccess::GetRefreshToken(Auth);
			// Force the refresh now (the timer would fire at 80% of a 15-min TTL). The
			// single-flight guard is exercised by the immediate second call — it must be
			// dropped, not revoke the session via double-presentation of the same token.
			FPlayServAuthTestAccess::ForceRefresh(Auth);
			FPlayServAuthTestAccess::ForceRefresh(Auth);
		}
		*bCaptured = true;
	}, bCaptured));

	// Settled = the rotation landed (token changed) or the session died (which the assert
	// below then reports as a failure).
	AddCommand(new FPlayServPollStep(this, TEXT("RefreshSettles"), [OldAccess]()
	{
		UPlayServAuth* Auth = AuthModule();
		return Auth == nullptr || !Auth->IsLoggedIn() || Auth->GetAccessToken() != *OldAccess;
	}, 10.0f));

	AddCommand(new FPlayServAssertStep(this, [PlayerId, OldAccess, OldRefresh](FAutomationTestBase* T)
	{
		UPlayServAuth* Auth = AuthModule();
		if (Auth == nullptr)
		{
			T->AddError(TEXT("Auth module unavailable"));
			return;
		}
		T->TestTrue(TEXT("still logged in after refresh (single-flight protected the family)"), Auth->IsLoggedIn());
		T->TestEqual(TEXT("player identity unchanged"), Auth->GetPlayerId(), *PlayerId);
		T->TestNotEqual(TEXT("access token rotated"), Auth->GetAccessToken(), *OldAccess);
		T->TestNotEqual(TEXT("refresh token rotated"), FPlayServAuthTestAccess::GetRefreshToken(Auth), *OldRefresh);
	}));

	AddV2LogoutStep(this);
	return true;
}

// ---------------------------------------------------------------------------
// SignOutRevokes — Logout revokes the session server-side: the refresh token captured
// before sign-out is dead afterwards (raw refresh with it → 401), and local state is
// cleared. A second Logout is a harmless no-op.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthV2SignOutRevokesTest,
	"PlayServ.Auth.V2.SignOutRevokes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthV2SignOutRevokesTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FString> PlayerId = MakeShared<FString>();
	TSharedPtr<FString> DeadRefresh = MakeShared<FString>();

	AddAnonLoginStep(this, PlayerId);

	TSharedPtr<bool> bCaptured = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("CaptureRefresh"), [bCaptured, DeadRefresh]()
	{
		if (UPlayServAuth* Auth = AuthModule())
		{
			*DeadRefresh = FPlayServAuthTestAccess::GetRefreshToken(Auth);
		}
		*bCaptured = true;
	}, bCaptured));

	AddV2LogoutStep(this);

	AddCommand(new FPlayServAssertStep(this, [](FAutomationTestBase* T)
	{
		UPlayServAuth* Auth = AuthModule();
		if (Auth != nullptr)
		{
			T->TestFalse(TEXT("logged out locally"), Auth->IsLoggedIn());
			T->TestTrue(TEXT("access token cleared"), Auth->GetAccessToken().IsEmpty());
		}
	}));

	// The server-side sign-out is async relative to the local clear — give it a moment,
	// then prove the family is dead: refreshing with the captured token must 401.
	AddCommand(new FPlayServDelayStep(2.0f));

	PlayServWireTest::FWireResultPtr Probe = MakeShared<PlayServWireTest::FWireResult>();
	TSharedPtr<bool> bProbed = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("DeadTokenProbe"), [Probe, DeadRefresh, bProbed]()
	{
		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("refresh_token"), *DeadRefresh);
		PlayServWireTest::Fire(TEXT("POST"), TEXT("/auth/players/refresh"), TEXT("client"), Body, Probe);
		*bProbed = true;
	}, bProbed));
	AddCommand(new FPlayServPollStep(this, TEXT("DeadTokenProbeWait"), [Probe]() { return Probe->bCompleted; }, 15.0f));

	AddCommand(new FPlayServAssertStep(this, [Probe](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("revoked refresh token is dead server-side (401)"), Probe->Status, 401);
	}));

	// Second logout: harmless no-op.
	AddV2LogoutStep(this);
	return true;
}

// ---------------------------------------------------------------------------
// LocalLogoutKeepsThePlayer — Logout(Local) ends the session in this process only: nothing is
// revoked, so the refresh token captured before it resumes the SAME player. It is the only way
// to sign out an anonymous player, whose refresh token is the one way back into the account.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthV2LocalLogoutKeepsThePlayerTest,
	"PlayServ.Auth.V2.LocalLogoutKeepsThePlayer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthV2LocalLogoutKeepsThePlayerTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FString> PlayerId = MakeShared<FString>();
	TSharedPtr<FString> KeptRefresh = MakeShared<FString>();

	AddAnonLoginStep(this, PlayerId);

	TSharedPtr<bool> bCaptured = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("CaptureRefresh"), [bCaptured, KeptRefresh]()
	{
		if (UPlayServAuth* Auth = AuthModule())
		{
			*KeptRefresh = FPlayServAuthTestAccess::GetRefreshToken(Auth);
		}
		*bCaptured = true;
	}, bCaptured));

	TSharedPtr<bool> bLoggedOut = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("LocalLogout"), [bLoggedOut]()
	{
		PlayServ::Auth::Logout(FPlayServSimpleCallback::CreateLambda(
			[bLoggedOut](bool, const FPlayServError&) { *bLoggedOut = true; }), EPlayServLogoutMode::Local);
	}, bLoggedOut, 8.0f));

	AddCommand(new FPlayServAssertStep(this, [](FAutomationTestBase* T)
	{
		UPlayServAuth* Auth = AuthModule();
		if (Auth != nullptr)
		{
			T->TestFalse(TEXT("logged out locally"), Auth->IsLoggedIn());
			T->TestTrue(TEXT("access token cleared"), Auth->GetAccessToken().IsEmpty());
		}
	}));

	// The wait SignOutRevokes gives a sign-out to land — here there must be nothing to land.
	AddCommand(new FPlayServDelayStep(2.0f));

	TSharedPtr<FString> ResumedId = MakeShared<FString>();
	TSharedPtr<FString> ResumeError = MakeShared<FString>();
	TSharedPtr<bool> bResumed = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("ResumeWithKeptToken"), [bResumed, KeptRefresh, ResumedId, ResumeError]()
	{
		PlayServ::Auth::LoginWithRefreshToken(*KeptRefresh, FPlayServAuthCallback::CreateLambda(
			[bResumed, ResumedId, ResumeError](bool bSuccess, const FString& InPlayerId, const FPlayServError& Error)
			{
				if (bSuccess)
				{
					*ResumedId = InPlayerId;
				}
				else
				{
					*ResumeError = Error.Message;
				}
				*bResumed = true;
			}));
	}, bResumed, 8.0f));

	AddCommand(new FPlayServAssertStep(this, [PlayerId, ResumedId, ResumeError](FAutomationTestBase* T)
	{
		T->TestTrue(FString::Printf(TEXT("the kept refresh token still resumes (%s)"), **ResumeError), !ResumedId->IsEmpty());
		T->TestEqual(TEXT("the same player comes back"), *ResumedId, *PlayerId);
	}));

	// Revoke on the way out: the test player's session does not outlive the test.
	AddV2LogoutStep(this);
	return true;
}

// ---------------------------------------------------------------------------
// LoginRequestShape — pins the external-login wire body WITHOUT a live provider token
// (an unattended run cannot mint an Epic token; live Epic verification stays a manual
// step). Synchronous, no network: asserts BuildV2LoginBody emits exactly
// {provider:"epic", provider_token:<token>} for EpicAccessToken, plus `display_name` when the
// caller set one (PSV-2646) — and nothing else rides along (no mode/nonce/fingerprint until an
// SDK surface offers them).
//
// AdditionalData is still asserted OFF the wire. It is unrelated dead surface on the same struct,
// deliberately out of PSV-2646's scope and carrying its own ticket, so the assertion that it does
// not ride along stays exactly as it was.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthV2LoginRequestShapeTest,
	"PlayServ.Auth.V2.LoginRequestShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthV2LoginRequestShapeTest::RunTest(const FString& Parameters)
{
	FPlayServExternalCredential Credential;
	Credential.Type = EPlayServExternalAuthType::EpicAccessToken;
	Credential.Token = TEXT("fake-eos-connect-token");
	Credential.AdditionalData.Add(TEXT("junk"), TEXT("must-not-ride-the-wire"));

	// No display name: the body is what it always was.
	{
		TSharedPtr<FJsonObject> Body = FPlayServAuthTestAccess::BuildV2LoginBody(Credential);
		if (!Body.IsValid())
		{
			AddError(TEXT("BuildV2LoginBody returned null"));
			return true;
		}

		TestEqual(TEXT("provider is the native EOS Connect arm"), Body->GetStringField(TEXT("provider")), FString(TEXT("epic")));
		TestEqual(TEXT("provider_token carries the credential token verbatim"), Body->GetStringField(TEXT("provider_token")), FString(TEXT("fake-eos-connect-token")));
		TestEqual(TEXT("body carries EXACTLY the two contract fields"), Body->Values.Num(), 2);
		TestFalse(TEXT("no launcher-exchange mode on the native-token arm"), Body->HasField(TEXT("mode")));
		TestFalse(TEXT("an unset display name sends no field at all"), Body->HasField(TEXT("display_name")));
		TestFalse(TEXT("AdditionalData does not ride the wire"), Body->HasField(TEXT("junk")));
	}

	// With one: exactly one more field, spelled snake_case. The spelling is the assertion that
	// matters — `displayName` and `name` are both accepted by the platform and both write nothing,
	// so a typo here is a silent loss rather than a failed request.
	{
		Credential.DisplayName = TEXT("Jane Q");

		TSharedPtr<FJsonObject> Body = FPlayServAuthTestAccess::BuildV2LoginBody(Credential);
		if (!Body.IsValid())
		{
			AddError(TEXT("BuildV2LoginBody returned null"));
			return true;
		}

		TestEqual(TEXT("display_name carries the name verbatim"), Body->GetStringField(TEXT("display_name")), FString(TEXT("Jane Q")));
		TestEqual(TEXT("body carries EXACTLY the three contract fields"), Body->Values.Num(), 3);
		TestFalse(TEXT("AdditionalData still does not ride the wire"), Body->HasField(TEXT("junk")));
	}
	return true;
}

// ---------------------------------------------------------------------------
// LoginRequestShapeLauncher — the launcher arm of the same pin.
//
// Together with the native assertion above, this is what "launcher code and EOS are mutually
// exclusive" means on the client: one credential carries one Type, one Type produces one body,
// and `mode` is present in exactly one of the two. The platform cannot be handed a request it
// could read two ways, because no such request can be built.
//
// `mode` must read exactly "launcher_exchange_code" — it is a wire enum with one legal value
// (contracts/api/components/schemas/auth.yaml, PlayerLoginRequest.mode), so a typo here is a
// 400, not a fallback to the native arm.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthV2LoginRequestShapeLauncherTest,
	"PlayServ.Auth.V2.LoginRequestShapeLauncher",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthV2LoginRequestShapeLauncherTest::RunTest(const FString& Parameters)
{
	FPlayServExternalCredential Credential;
	Credential.Type = EPlayServExternalAuthType::EpicLauncherExchangeCode;
	Credential.Token = TEXT("6f0e1c9a4b2d47f8a1c3e5d7b9f0a2c4");
	Credential.AdditionalData.Add(TEXT("junk"), TEXT("must-not-ride-the-wire"));

	{
		TSharedPtr<FJsonObject> Body = FPlayServAuthTestAccess::BuildV2LoginBody(Credential);
		if (!Body.IsValid())
		{
			AddError(TEXT("BuildV2LoginBody returned null"));
			return true;
		}

		TestEqual(TEXT("provider is still epic — the arm is chosen by mode, not by provider"),
			Body->GetStringField(TEXT("provider")), FString(TEXT("epic")));
		TestEqual(TEXT("provider_token carries the exchange code verbatim"),
			Body->GetStringField(TEXT("provider_token")), FString(TEXT("6f0e1c9a4b2d47f8a1c3e5d7b9f0a2c4")));
		TestEqual(TEXT("mode is exactly the one legal wire value"),
			Body->GetStringField(TEXT("mode")), FString(TEXT("launcher_exchange_code")));
		TestEqual(TEXT("body carries EXACTLY the three contract fields"), Body->Values.Num(), 3);
		TestFalse(TEXT("an unset display name sends no field at all"), Body->HasField(TEXT("display_name")));
		TestFalse(TEXT("AdditionalData does not ride the wire"), Body->HasField(TEXT("junk")));
	}

	// The launcher arm is the one that matters most for a name: the exchange code resolves to an
	// Epic account the platform names "Epic Player" on its own, so a login that drops the real
	// nickname produces exactly the blank-looking records PSV-2646 exists to fix.
	{
		Credential.DisplayName = TEXT("LauncherJane");

		TSharedPtr<FJsonObject> Body = FPlayServAuthTestAccess::BuildV2LoginBody(Credential);
		if (!Body.IsValid())
		{
			AddError(TEXT("BuildV2LoginBody returned null"));
			return true;
		}

		TestEqual(TEXT("display_name rides alongside mode"), Body->GetStringField(TEXT("display_name")), FString(TEXT("LauncherJane")));
		TestEqual(TEXT("body carries EXACTLY the four contract fields"), Body->Values.Num(), 4);
		TestFalse(TEXT("AdditionalData still does not ride the wire"), Body->HasField(TEXT("junk")));
	}
	return true;
}

// ---------------------------------------------------------------------------
// LoginRequestShapeSteam — the Steam arm of the same pin.
//
// Steam goes through the SAME shared native door as Epic, discriminated by `provider` alone.
// The absence of `mode` is the assertion that matters: Steam has one native login shape, and
// `mode` is a wire enum whose only legal value is launcher_exchange_code
// (contracts/api/components/schemas/auth.yaml, PlayerLoginRequest.mode), so a stray `mode`
// here would be a 422 rather than a hint. The platform's `steam` arm ignores `mode` entirely
// (PlayerAuthFlowService.VerifyProviderTokenAsync matches `(ProviderId.Steam, _)`), which is
// exactly why sending one would be an error the wire never reports back.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthV2LoginRequestShapeSteamTest,
	"PlayServ.Auth.V2.LoginRequestShapeSteam",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthV2LoginRequestShapeSteamTest::RunTest(const FString& Parameters)
{
	FPlayServExternalCredential Credential;
	Credential.Type = EPlayServExternalAuthType::SteamWebApiTicket;
	Credential.Token = TEXT("140000000100ABFF");
	Credential.AdditionalData.Add(TEXT("junk"), TEXT("must-not-ride-the-wire"));

	{
		TSharedPtr<FJsonObject> Body = FPlayServAuthTestAccess::BuildV2LoginBody(Credential);
		if (!Body.IsValid())
		{
			AddError(TEXT("BuildV2LoginBody returned null"));
			return true;
		}

		TestEqual(TEXT("provider names the Steam ticket arm"),
			Body->GetStringField(TEXT("provider")), FString(TEXT("steam")));
		TestEqual(TEXT("provider_token carries the hex ticket verbatim"),
			Body->GetStringField(TEXT("provider_token")), FString(TEXT("140000000100ABFF")));
		TestEqual(TEXT("body carries EXACTLY the two contract fields"), Body->Values.Num(), 2);
		TestFalse(TEXT("no mode on the Steam arm — it has only one native login shape"),
			Body->HasField(TEXT("mode")));
		TestFalse(TEXT("an unset display name sends no field at all"), Body->HasField(TEXT("display_name")));
		TestFalse(TEXT("AdditionalData does not ride the wire"), Body->HasField(TEXT("junk")));
	}

	// display_name is provider-independent: it is emitted for EVERY credential type, so a new arm
	// added later inherits the behaviour instead of having to remember it.
	{
		Credential.DisplayName = TEXT("SteamJane");

		TSharedPtr<FJsonObject> Body = FPlayServAuthTestAccess::BuildV2LoginBody(Credential);
		if (!Body.IsValid())
		{
			AddError(TEXT("BuildV2LoginBody returned null"));
			return true;
		}

		TestEqual(TEXT("display_name carries the name verbatim"), Body->GetStringField(TEXT("display_name")), FString(TEXT("SteamJane")));
		TestEqual(TEXT("body carries EXACTLY the three contract fields"), Body->Values.Num(), 3);
		TestFalse(TEXT("still no mode on the Steam arm"), Body->HasField(TEXT("mode")));
		TestFalse(TEXT("AdditionalData still does not ride the wire"), Body->HasField(TEXT("junk")));
	}
	return true;
}

// ---------------------------------------------------------------------------
// LoginExternal fail-fast guards — no network call, synchronous error callback.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthV2LoginRejectsUnsupportedTypeTest,
	"PlayServ.Auth.V2.LoginRejectsUnsupportedType",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthV2LoginRejectsUnsupportedTypeTest::RunTest(const FString& Parameters)
{
	FPlayServExternalCredential Credential;
	Credential.Type = EPlayServExternalAuthType::None;
	Credential.Token = TEXT("token-that-must-not-be-sent");

	TSharedPtr<bool> bFired = MakeShared<bool>(false);
	TSharedPtr<bool> bSuccess = MakeShared<bool>(true);
	PlayServ::Auth::LoginExternal(Credential, FPlayServAuthCallback::CreateLambda(
		[bFired, bSuccess](bool bOk, const FString&, const FPlayServError&)
		{
			*bSuccess = bOk;
			*bFired = true;
		}));

	TestTrue(TEXT("unsupported type fails fast (synchronous callback, no network)"), *bFired);
	TestFalse(TEXT("unsupported type reports failure"), *bSuccess);
	UPlayServAuth* Auth = AuthModule();
	if (Auth != nullptr)
	{
		TestFalse(TEXT("no session was started"), Auth->IsLoggedIn());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthV2LoginRejectsEmptyTokenTest,
	"PlayServ.Auth.V2.LoginRejectsEmptyToken",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthV2LoginRejectsEmptyTokenTest::RunTest(const FString& Parameters)
{
	FPlayServExternalCredential Credential;
	Credential.Type = EPlayServExternalAuthType::EpicAccessToken;
	Credential.Token = TEXT("");

	TSharedPtr<bool> bFired = MakeShared<bool>(false);
	TSharedPtr<bool> bSuccess = MakeShared<bool>(true);
	PlayServ::Auth::LoginExternal(Credential, FPlayServAuthCallback::CreateLambda(
		[bFired, bSuccess](bool bOk, const FString&, const FPlayServError&)
		{
			*bSuccess = bOk;
			*bFired = true;
		}));

	TestTrue(TEXT("empty token fails fast (synchronous callback, no network)"), *bFired);
	TestFalse(TEXT("empty token reports failure"), *bSuccess);
	return true;
}

// The launcher arm is a second value in the type gate. Proving it reaches the shared
// empty-token guard proves it was admitted by the gate rather than slipping past it — a value
// the gate rejected would fail here too, but with the wrong reason, so both are asserted.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthV2LoginRejectsEmptyExchangeCodeTest,
	"PlayServ.Auth.V2.LoginRejectsEmptyExchangeCode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthV2LoginRejectsEmptyExchangeCodeTest::RunTest(const FString& Parameters)
{
	FPlayServExternalCredential Credential;
	Credential.Type = EPlayServExternalAuthType::EpicLauncherExchangeCode;
	Credential.Token = TEXT("");

	TSharedPtr<bool> bFired = MakeShared<bool>(false);
	TSharedPtr<bool> bSuccess = MakeShared<bool>(true);
	TSharedPtr<FString> Message = MakeShared<FString>();
	PlayServ::Auth::LoginExternal(Credential, FPlayServAuthCallback::CreateLambda(
		[bFired, bSuccess, Message](bool bOk, const FString&, const FPlayServError& Error)
		{
			*bSuccess = bOk;
			*Message = Error.Message;
			*bFired = true;
		}));

	TestTrue(TEXT("empty exchange code fails fast (synchronous callback, no network)"), *bFired);
	TestFalse(TEXT("empty exchange code reports failure"), *bSuccess);
	TestFalse(TEXT("it failed on the EMPTY-TOKEN guard, not on the type gate"),
		Message->Contains(TEXT("not yet supported")));
	return true;
}

// The Steam arm is the third value in the type gate, and the only one carrying a
// shape check of its own. Both halves are asserted for the same reason as the launcher case
// above: a value the gate rejected would also fail here, but for the wrong reason.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthV2LoginRejectsNonHexSteamTicketTest,
	"PlayServ.Auth.V2.LoginRejectsNonHexSteamTicket",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthV2LoginRejectsNonHexSteamTicketTest::RunTest(const FString& Parameters)
{
	FPlayServExternalCredential Credential;
	Credential.Type = EPlayServExternalAuthType::SteamWebApiTicket;
	// Non-empty, so it clears the shared empty-token guard and reaches the Steam-specific one.
	Credential.Token = TEXT("not-a-hex-ticket");

	TSharedPtr<bool> bFired = MakeShared<bool>(false);
	TSharedPtr<bool> bSuccess = MakeShared<bool>(true);
	TSharedPtr<FString> Message = MakeShared<FString>();
	PlayServ::Auth::LoginExternal(Credential, FPlayServAuthCallback::CreateLambda(
		[bFired, bSuccess, Message](bool bOk, const FString&, const FPlayServError& Error)
		{
			*bSuccess = bOk;
			*Message = Error.Message;
			*bFired = true;
		}));

	TestTrue(TEXT("a non-hex Steam ticket fails fast (synchronous callback, no network)"), *bFired);
	TestFalse(TEXT("a non-hex Steam ticket reports failure"), *bSuccess);
	TestFalse(TEXT("it failed on the HEX guard, not on the type gate — the Steam arm is admitted"),
		Message->Contains(TEXT("not yet supported")));
	TestTrue(TEXT("the error names the acquisition call, so the fix is readable from the message"),
		Message->Contains(TEXT("GetAuthTicketForWebApi")));
	return true;
}

// ---------------------------------------------------------------------------
// LoginSendsNoPlayerBearer — regression guard for a bearer leak found during live Epic
// verification.
//
// The platform resolves the caller of POST /auth/players/login from the Authorization
// bearer, and a non-null caller turns a LOGIN into a provider LINK
// (PlayerRuntimeAuthController.Login -> callerPlayerId). Confirmed live: with an anonymous
// session live, a valid Epic token came back provider_already_linked (current = the guest
// plr_, conflicting = the real Epic plr_) instead of logging in. LoginExternal must send no
// player bearer even when a session exists.
//
// Asserted at the transport on purpose: PlayServ.Auth.V2.LoginRequestShape calls
// BuildV2LoginBody directly and structurally cannot observe a header, so it can never catch
// this. The seam records only WHETHER a header was set, never its value.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthV2LoginSendsNoPlayerBearerTest,
	"PlayServ.Auth.V2.LoginSendsNoPlayerBearer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthV2LoginSendsNoPlayerBearerTest::RunTest(const FString& Parameters)
{
	// A live client session is the precondition: the leak only exists when there is a bearer
	// to leak. This is the guest-then-Epic upgrade path a real game ships.
	TSharedPtr<FString> GuestId = MakeShared<FString>();
	AddAnonLoginStep(this, GuestId);

	AddCommand(new FPlayServAssertStep(this, [GuestId](FAutomationTestBase* T)
	{
		UPlayServAuth* Auth = AuthModule();
		T->TestTrue(TEXT("precondition: a guest session is live"), Auth != nullptr && Auth->IsLoggedIn());
		T->TestTrue(TEXT("precondition: a bearer exists to leak"), Auth != nullptr && !Auth->GetAccessToken().IsEmpty());
	}));

	// The token is deliberately bogus — the platform will reject it, and that is fine. What
	// is under test is the OUTGOING request's headers, not the response.
	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("LoginExternalWithSessionLive"), [bDone]()
	{
		FPlayServExternalCredential Credential;
		Credential.Type = EPlayServExternalAuthType::EpicAccessToken;
		Credential.Token = TEXT("not-a-real-epic-token-headers-are-what-matter");

		PlayServ::Auth::LoginExternal(Credential, FPlayServAuthCallback::CreateLambda(
			[bDone](bool, const FString&, const FPlayServError&) { *bDone = true; }));
	}, bDone, 12.0f));

	AddCommand(new FPlayServAssertStep(this, [](FAutomationTestBase* T)
	{
		const FPlayServHttp::FLastRequestForTest& Last = FPlayServHttp::GetLastRequestForTest();

		// Fail loudly rather than vacuously if some other request landed last.
		T->TestEqual(TEXT("the last dispatched request was the login POST"), Last.Path, FString(TEXT("/auth/players/login")));
		T->TestEqual(TEXT("verb is POST"), Last.Verb, FString(TEXT("POST")));
		T->TestTrue(TEXT("client plane key was sent"), Last.bHasClientKey);
		T->TestFalse(TEXT("NO player bearer on the login request (a bearer would make it a LINK)"), Last.bHasAuthorization);
	}));

	AddV2LogoutStep(this);
	return true;
}

// ---------------------------------------------------------------------------
// PlayerLoginAfterServerSession — a player login made while a server session is live goes out on the client plane.
//
// The transport picked the plane from the live session, so an anonymous login after LoginServer carried the server
// key; the platform refused it 403 forbidden_for_credential and the process stayed on the server session, where every
// later "client" call ran with server rights.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthV2PlayerLoginAfterServerSessionTest,
	"PlayServ.Auth.V2.PlayerLoginAfterServerSession",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthV2PlayerLoginAfterServerSessionTest::RunTest(const FString& Parameters)
{
	PlayServWireTest::AddServerLoginStep(this);

	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	TSharedPtr<bool> bOk = MakeShared<bool>(false);
	TSharedPtr<FPlayServError> Error = MakeShared<FPlayServError>();
	AddCommand(new FPlayServAsyncStep(this, TEXT("LoginAnonymousFromServerSession"), [bDone, bOk, Error]()
	{
		PlayServ::Auth::LoginAnonymous(MakeSuiteDisplayName(TEXT("PlayerLoginAfterServerSession")), FPlayServAuthCallback::CreateLambda(
			[bDone, bOk, Error](bool bSuccess, const FString&, const FPlayServError& InError)
			{
				*bOk = bSuccess;
				*Error = InError;
				*bDone = true;
			}));
	}, bDone, 8.0f));

	AddCommand(new FPlayServAssertStep(this, [bOk, Error](FAutomationTestBase* T)
	{
		T->TestTrue(FString::Printf(TEXT("an anonymous login from a server session succeeds (%s %s)"), *Error->ProblemCode, *Error->Message), *bOk);
		T->TestFalse(TEXT("the process is on a client session after it"), PlayServ::Auth::IsServerSession());
		T->TestFalse(TEXT("the new session has a player id"), PlayServ::Auth::GetPlayerId().IsEmpty());
	}));
	return true;
}

// ---------------------------------------------------------------------------
// PSV-2644 — anonymous session continuity.
//
// LoginAnonymous mints a new plr_* every time, so a guest identity survives a relaunch only
// through LoginWithRefreshToken: the host persists the rotating refresh token and presents it
// on the next cold start. The tests below cover the decision that can be wrong silently (the
// player id, which the refresh response does not carry), the guard that must not spend a round
// trip, the resume itself, the terminal-failure path, and the persistence hook.
// ---------------------------------------------------------------------------

namespace
{
	/**
	 * Build a synthetic session access token: three base64url segments, the middle one the given
	 * JSON payload. Header and signature are filler — which is the point, since the SDK must not
	 * be verifying either.
	 */
	FString MakeSyntheticAccessToken(const FString& PayloadJson, bool bStripPadding = true)
	{
		FString Payload = FBase64::Encode(PayloadJson, EBase64Mode::UrlSafe);
		if (bStripPadding)
		{
			// Real JWTs are unpadded (RFC 7515 2). FBase64::Encode pads, so strip it to reach the
			// shape the platform actually sends.
			Payload.RemoveFromEnd(TEXT("="));
			Payload.RemoveFromEnd(TEXT("="));
		}
		FString Header = FBase64::Encode(FString(TEXT("{\"alg\":\"RS256\",\"typ\":\"JWT\"}")), EBase64Mode::UrlSafe);
		Header.ReplaceInline(TEXT("="), TEXT(""));
		return FString::Printf(TEXT("%s.%s.%s"), *Header, *Payload, TEXT("c2lnbmF0dXJl"));
	}
}

// ---------------------------------------------------------------------------
// PlayerIdFromAccessToken — the one piece of new logic that fails silently.
//
// The refresh response carries no player_id by contract, so LoginWithRefreshToken derives it
// from the access token's `sub` claim. Get that wrong and the login still "succeeds" — with an
// empty GetPlayerId(), and every player-keyed call misbehaving quietly from then on.
// Synchronous, no network: a real token cannot be minted headlessly, and none is needed.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthV2PlayerIdFromAccessTokenTest,
	"PlayServ.Auth.V2.PlayerIdFromAccessToken",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthV2PlayerIdFromAccessTokenTest::RunTest(const FString& Parameters)
{
	// The shape the platform sends: unpadded base64url, sub alongside the other session claims.
	{
		FString PlayerId;
		const FString Token = MakeSyntheticAccessToken(
			TEXT("{\"sub\":\"plr_01HQZX8N4K\",\"project_id\":\"prj_1\",\"env\":\"dev\",\"session_id\":\"ses_9\"}"));
		TestTrue(TEXT("unpadded base64url payload decodes"),
			FPlayServAuthTestAccess::TryParsePlayerIdFromAccessToken(Token, PlayerId));
		TestEqual(TEXT("sub is read verbatim"), PlayerId, FString(TEXT("plr_01HQZX8N4K")));
	}

	// Padded base64url is legal too — accept it rather than depend on the issuer's padding habit.
	{
		FString PlayerId;
		const FString Token = MakeSyntheticAccessToken(TEXT("{\"sub\":\"plr_padded\"}"), /*bStripPadding=*/false);
		TestTrue(TEXT("padded base64url payload decodes"),
			FPlayServAuthTestAccess::TryParsePlayerIdFromAccessToken(Token, PlayerId));
		TestEqual(TEXT("sub is read verbatim (padded)"), PlayerId, FString(TEXT("plr_padded")));
	}

	// Every rejection below must leave OutPlayerId untouched — a half-written id is worse than
	// none, because the caller's own "is it empty" check would then pass.
	{
		FString PlayerId = TEXT("untouched");
		TestFalse(TEXT("an empty token is not a JWT"),
			FPlayServAuthTestAccess::TryParsePlayerIdFromAccessToken(TEXT(""), PlayerId));
		TestFalse(TEXT("a non-JWT string is rejected"),
			FPlayServAuthTestAccess::TryParsePlayerIdFromAccessToken(TEXT("not-a-jwt"), PlayerId));
		TestFalse(TEXT("an sk_ server key is not a JWT"),
			FPlayServAuthTestAccess::TryParsePlayerIdFromAccessToken(TEXT("sk_abcdef0123456789"), PlayerId));
		TestFalse(TEXT("a payload that is not JSON is rejected"),
			FPlayServAuthTestAccess::TryParsePlayerIdFromAccessToken(MakeSyntheticAccessToken(TEXT("plain text, not json")), PlayerId));
		TestFalse(TEXT("a JSON payload with no sub is rejected"),
			FPlayServAuthTestAccess::TryParsePlayerIdFromAccessToken(MakeSyntheticAccessToken(TEXT("{\"project_id\":\"prj_1\"}")), PlayerId));
		TestFalse(TEXT("an empty sub is rejected"),
			FPlayServAuthTestAccess::TryParsePlayerIdFromAccessToken(MakeSyntheticAccessToken(TEXT("{\"sub\":\"\"}")), PlayerId));
		TestFalse(TEXT("a non-string sub is rejected"),
			FPlayServAuthTestAccess::TryParsePlayerIdFromAccessToken(MakeSyntheticAccessToken(TEXT("{\"sub\":42}")), PlayerId));
		TestEqual(TEXT("OutPlayerId is untouched on every rejection"), PlayerId, FString(TEXT("untouched")));
	}

	return true;
}

// ---------------------------------------------------------------------------
// LoginWithRefreshTokenRejectsEmptyToken — fail fast, synchronously, no round trip.
//
// Mirrors the LoginExternal empty-token guard. Worth a test of its own because without the
// guard the callback becomes asynchronous, and a caller that relies on the fall-through-to-
// anonymous ordering (BlobEater's login-by-name does) would fire two logins.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthV2LoginWithRefreshTokenRejectsEmptyTokenTest,
	"PlayServ.Auth.V2.LoginWithRefreshTokenRejectsEmptyToken",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthV2LoginWithRefreshTokenRejectsEmptyTokenTest::RunTest(const FString& Parameters)
{
	TSharedPtr<bool> bFired = MakeShared<bool>(false);
	TSharedPtr<bool> bSuccess = MakeShared<bool>(true);
	TSharedPtr<FString> ReportedPlayerId = MakeShared<FString>(TEXT("unset"));

	PlayServ::Auth::LoginWithRefreshToken(TEXT(""), FPlayServAuthCallback::CreateLambda(
		[bFired, bSuccess, ReportedPlayerId](bool bOk, const FString& PlayerId, const FPlayServError&)
		{
			*bSuccess = bOk;
			*ReportedPlayerId = PlayerId;
			*bFired = true;
		}));

	TestTrue(TEXT("empty refresh token fails fast (synchronous callback, no network)"), *bFired);
	TestFalse(TEXT("empty refresh token reports failure"), *bSuccess);
	TestTrue(TEXT("no player id is reported on failure"), ReportedPlayerId->IsEmpty());

	UPlayServAuth* Auth = AuthModule();
	if (Auth != nullptr)
	{
		TestFalse(TEXT("no session was started"), Auth->IsLoggedIn());
	}
	return true;
}

// ---------------------------------------------------------------------------
// ResumedSessionKeepsSamePlayer — the regression this ticket exists for.
//
// Anonymous login, capture the refresh token the host would have persisted, drop all local
// session state WITHOUT signing out (the local half of a process restart — the session is still
// alive server-side), then log in with the stored token and land on the SAME plr_. When this
// goes red, a relaunching player has silently become somebody else and lost their profile.
//
// Live against dev on purpose: the defect class lives in the real request/response path — an id
// the response does not carry, and a token the platform rotates and may revoke. A fake at that
// seam would prove nothing about either.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthV2ResumedSessionKeepsSamePlayerTest,
	"PlayServ.Auth.V2.ResumedSessionKeepsSamePlayer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthV2ResumedSessionKeepsSamePlayerTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FString> OriginalPlayerId = MakeShared<FString>();
	TSharedPtr<FString> StoredRefresh = MakeShared<FString>();
	TSharedPtr<FString> ResumedPlayerId = MakeShared<FString>();
	TSharedPtr<bool> bResumeOk = MakeShared<bool>(false);
	TSharedPtr<FString> ResumeError = MakeShared<FString>();

	AddAnonLoginStep(this, OriginalPlayerId);

	// What the host's SetRefreshTokenChangedHandler would have written to storage, followed by
	// the process dying: local state gone, server-side session untouched.
	TSharedPtr<bool> bRestarted = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("StoreTokenAndRestart"), [bRestarted, StoredRefresh]()
	{
		if (UPlayServAuth* Auth = AuthModule())
		{
			*StoredRefresh = FPlayServAuthTestAccess::GetRefreshToken(Auth);
			FPlayServAuthTestAccess::SimulateProcessRestart(Auth);
		}
		*bRestarted = true;
	}, bRestarted));

	AddCommand(new FPlayServAssertStep(this, [StoredRefresh](FAutomationTestBase* T)
	{
		UPlayServAuth* Auth = AuthModule();
		T->TestTrue(TEXT("a refresh token was there to store"), !StoredRefresh->IsEmpty());
		if (Auth != nullptr)
		{
			T->TestFalse(TEXT("cold start: no local session"), Auth->IsLoggedIn());
			T->TestTrue(TEXT("cold start: no player id"), Auth->GetPlayerId().IsEmpty());
		}
	}));

	TSharedPtr<bool> bResumed = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("LoginWithRefreshToken"),
		[bResumed, StoredRefresh, ResumedPlayerId, bResumeOk, ResumeError]()
	{
		PlayServ::Auth::LoginWithRefreshToken(*StoredRefresh, FPlayServAuthCallback::CreateLambda(
			[bResumed, ResumedPlayerId, bResumeOk, ResumeError](bool bSuccess, const FString& PlayerId, const FPlayServError& Error)
			{
				*bResumeOk = bSuccess;
				*ResumedPlayerId = PlayerId;
				*ResumeError = Error.Message;
				*bResumed = true;
			}));
	}, bResumed, 10.0f));

	AddCommand(new FPlayServAssertStep(this,
		[OriginalPlayerId, ResumedPlayerId, bResumeOk, ResumeError, StoredRefresh](FAutomationTestBase* T)
	{
		UPlayServAuth* Auth = AuthModule();
		if (Auth == nullptr)
		{
			T->AddError(TEXT("Auth module unavailable"));
			return;
		}
		T->TestTrue(FString::Printf(TEXT("resume succeeded (%s)"), **ResumeError), *bResumeOk);
		T->TestTrue(TEXT("resumed player id is plr_*"), ResumedPlayerId->StartsWith(TEXT("plr_")));
		T->TestEqual(TEXT("SAME player as before the restart"), *ResumedPlayerId, *OriginalPlayerId);
		T->TestEqual(TEXT("GetPlayerId matches the callback — not empty"), Auth->GetPlayerId(), *ResumedPlayerId);
		T->TestTrue(TEXT("session is live"), Auth->IsLoggedIn());
		T->TestEqual(TEXT("session type is Client"), Auth->GetSessionType(), EPlayServSessionType::Client);
		T->TestTrue(TEXT("access token stored"), !Auth->GetAccessToken().IsEmpty());
		T->TestTrue(TEXT("refresh timer has a TTL to run on"), FPlayServAuthTestAccess::GetAccessTokenTTL(Auth) > 0);
		T->TestNotEqual(TEXT("the presented token was rotated — the stored copy is now spent"),
			FPlayServAuthTestAccess::GetRefreshToken(Auth), *StoredRefresh);
	}));

	AddV2LogoutStep(this);
	return true;
}

// ---------------------------------------------------------------------------
// ResumeWithRevokedTokenFails — a dead token fails cleanly and strands nobody.
//
// Sign-out kills the whole family, so the copy the host persisted before it is worthless. The
// caller must get a failure it can act on (discard the entry, start a fresh guest) rather than a
// half-session: no player id, no logged-in state, nothing for a later call to key on. A
// 30-day-expired token takes this same path — the platform answers both with the same 401.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthV2ResumeWithRevokedTokenFailsTest,
	"PlayServ.Auth.V2.ResumeWithRevokedTokenFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthV2ResumeWithRevokedTokenFailsTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FString> PlayerId = MakeShared<FString>();
	TSharedPtr<FString> DeadRefresh = MakeShared<FString>();

	AddAnonLoginStep(this, PlayerId);

	TSharedPtr<bool> bCaptured = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("CaptureRefresh"), [bCaptured, DeadRefresh]()
	{
		if (UPlayServAuth* Auth = AuthModule())
		{
			*DeadRefresh = FPlayServAuthTestAccess::GetRefreshToken(Auth);
		}
		*bCaptured = true;
	}, bCaptured));

	// Sign out — the family the stored token belongs to is now revoked server-side.
	AddV2LogoutStep(this);
	AddCommand(new FPlayServDelayStep(2.0f));

	TSharedPtr<bool> bAttempted = MakeShared<bool>(false);
	TSharedPtr<bool> bResumeOk = MakeShared<bool>(true);
	TSharedPtr<FString> ReportedPlayerId = MakeShared<FString>(TEXT("unset"));
	AddCommand(new FPlayServAsyncStep(this, TEXT("ResumeWithDeadToken"),
		[bAttempted, DeadRefresh, bResumeOk, ReportedPlayerId]()
	{
		PlayServ::Auth::LoginWithRefreshToken(*DeadRefresh, FPlayServAuthCallback::CreateLambda(
			[bAttempted, bResumeOk, ReportedPlayerId](bool bSuccess, const FString& InPlayerId, const FPlayServError&)
			{
				*bResumeOk = bSuccess;
				*ReportedPlayerId = InPlayerId;
				*bAttempted = true;
			}));
	}, bAttempted, 10.0f));

	AddCommand(new FPlayServAssertStep(this, [bResumeOk, ReportedPlayerId](FAutomationTestBase* T)
	{
		T->TestFalse(TEXT("a revoked token cannot resume a session"), *bResumeOk);
		T->TestTrue(TEXT("no player id is reported"), ReportedPlayerId->IsEmpty());

		UPlayServAuth* Auth = AuthModule();
		if (Auth != nullptr)
		{
			T->TestFalse(TEXT("no session was started"), Auth->IsLoggedIn());
			T->TestTrue(TEXT("caller is not stranded mid-session — player id stays empty"), Auth->GetPlayerId().IsEmpty());
		}
	}));

	return true;
}

// ---------------------------------------------------------------------------
// RefreshTokenHandlerFires — the persistence hook, at login and on rotation.
//
// The other half of continuity: a host told the token only at login stores a value that is spent
// ~12 minutes later, and the next launch resumes into a 401. So the hook must fire on BOTH
// events, handing over the token current at that moment.
//
// It must also stay quiet where firing would destroy the stored copy: the handler is not told
// about logout, because the same local-clear path runs on ordinary shutdown.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthV2RefreshTokenHandlerFiresTest,
	"PlayServ.Auth.V2.RefreshTokenHandlerFires",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthV2RefreshTokenHandlerFiresTest::RunTest(const FString& Parameters)
{
	TSharedPtr<TArray<FString>> Handed = MakeShared<TArray<FString>>();

	TSharedPtr<bool> bInstalled = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("InstallHandler"), [bInstalled, Handed]()
	{
		PlayServ::Auth::SetRefreshTokenChangedHandler(FPlayServRefreshTokenChanged::CreateLambda(
			[Handed](const FString& RefreshToken) { Handed->Add(RefreshToken); }));
		*bInstalled = true;
	}, bInstalled));

	TSharedPtr<FString> PlayerId = MakeShared<FString>();
	AddAnonLoginStep(this, PlayerId);

	AddCommand(new FPlayServAssertStep(this, [Handed](FAutomationTestBase* T)
	{
		UPlayServAuth* Auth = AuthModule();
		T->TestEqual(TEXT("handler fired once at login"), Handed->Num(), 1);
		if (Handed->Num() == 1 && Auth != nullptr)
		{
			T->TestTrue(TEXT("the token handed over is non-empty"), !(*Handed)[0].IsEmpty());
			T->TestEqual(TEXT("it is the session's current refresh token"),
				(*Handed)[0], FPlayServAuthTestAccess::GetRefreshToken(Auth));
		}
	}));

	TSharedPtr<FString> PreRotation = MakeShared<FString>();
	TSharedPtr<bool> bForced = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("ForceRotation"), [bForced, PreRotation]()
	{
		if (UPlayServAuth* Auth = AuthModule())
		{
			*PreRotation = FPlayServAuthTestAccess::GetRefreshToken(Auth);
			FPlayServAuthTestAccess::ForceRefresh(Auth);
		}
		*bForced = true;
	}, bForced));

	AddCommand(new FPlayServPollStep(this, TEXT("RotationSettles"), [PreRotation]()
	{
		UPlayServAuth* Auth = AuthModule();
		return Auth == nullptr || !Auth->IsLoggedIn()
			|| FPlayServAuthTestAccess::GetRefreshToken(Auth) != *PreRotation;
	}, 10.0f));

	AddCommand(new FPlayServAssertStep(this, [Handed](FAutomationTestBase* T)
	{
		UPlayServAuth* Auth = AuthModule();
		T->TestEqual(TEXT("handler fired again on rotation"), Handed->Num(), 2);
		if (Handed->Num() == 2 && Auth != nullptr)
		{
			T->TestNotEqual(TEXT("the rotated token is a new value"), (*Handed)[1], (*Handed)[0]);
			T->TestEqual(TEXT("it is the session's current refresh token"),
				(*Handed)[1], FPlayServAuthTestAccess::GetRefreshToken(Auth));
		}
	}));

	AddV2LogoutStep(this);

	AddCommand(new FPlayServAssertStep(this, [Handed](FAutomationTestBase* T)
	{
		// Firing here would mean firing on ordinary shutdown too, wiping the stored token the
		// next launch needs. Logout and OnSessionLost are the game's cue to clear, not this.
		T->TestEqual(TEXT("handler is NOT told about logout"), Handed->Num(), 2);
	}));

	// Leave no handler installed for whatever runs next in this process.
	TSharedPtr<bool> bCleared = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("ClearHandler"), [bCleared]()
	{
		PlayServ::Auth::SetRefreshTokenChangedHandler(FPlayServRefreshTokenChanged());
		*bCleared = true;
	}, bCleared));

	return true;
}

// ---------------------------------------------------------------------------
// DisplayNameSanitizing — the SDK is the ONLY guard on this value (PSV-2646).
//
// Probed against dev: the platform stores 5000 characters verbatim, preserves padding, and
// treats a whitespace-only name as absent. So nothing downstream will catch a bad value —
// a wrong answer here is not a failed request, it is a wrong name in the operator console.
// Synchronous, no network.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthV2DisplayNameSanitizingTest,
	"PlayServ.Auth.V2.DisplayNameSanitizing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthV2DisplayNameSanitizingTest::RunTest(const FString& Parameters)
{
	// Trimming. The platform PRESERVES padding, so an untrimmed name arrives with its spaces —
	// and "  " arrives as a name that is then stored as nothing at all.
	TestEqual(TEXT("leading and trailing whitespace is trimmed"),
		FPlayServAuthTestAccess::SanitizeDisplayName(TEXT("   Jane Q   ")), FString(TEXT("Jane Q")));
	TestEqual(TEXT("tabs and newlines count as whitespace"),
		FPlayServAuthTestAccess::SanitizeDisplayName(TEXT("\t\r\nJane\n")), FString(TEXT("Jane")));
	TestEqual(TEXT("interior whitespace is left alone — it is part of the name"),
		FPlayServAuthTestAccess::SanitizeDisplayName(TEXT("Jane  Q  Public")), FString(TEXT("Jane  Q  Public")));
	TestTrue(TEXT("a whitespace-only name sanitises to nothing, so no field is sent"),
		FPlayServAuthTestAccess::SanitizeDisplayName(TEXT("   \t  ")).IsEmpty());
	TestTrue(TEXT("an empty name stays empty"),
		FPlayServAuthTestAccess::SanitizeDisplayName(TEXT("")).IsEmpty());

	// The 64 boundary, from both sides.
	{
		const FString Exactly64 = FString::ChrN(64, TEXT('A'));
		TestEqual(TEXT("exactly 64 characters passes through untouched"),
			FPlayServAuthTestAccess::SanitizeDisplayName(Exactly64), Exactly64);
		TestEqual(TEXT("exactly 64 is still 64"),
			FPlayServAuthTestAccess::SanitizeDisplayName(Exactly64).Len(), 64);
	}
	{
		// 65 is TRUNCATED, not rejected: a provider nickname one character too long must not be
		// able to fail a sign-in. This is the credential/cosmetic split the SDK already draws.
		AddExpectedError(TEXT("display name truncated"), EAutomationExpectedMessageFlags::Contains, 0);
		const FString Result = FPlayServAuthTestAccess::SanitizeDisplayName(FString::ChrN(65, TEXT('B')));
		TestEqual(TEXT("65 characters truncate to 64"), Result.Len(), 64);
		TestEqual(TEXT("truncation keeps the leading 64, it does not reject"), Result, FString::ChrN(64, TEXT('B')));
	}
	{
		// Padding is trimmed BEFORE the cap, so a padded 64 survives whole rather than losing its
		// last real character to the spaces.
		const FString Padded = FString(TEXT("  ")) + FString::ChrN(64, TEXT('C')) + FString(TEXT("  "));
		TestEqual(TEXT("trim runs before the cap, so a padded 64 is not truncated"),
			FPlayServAuthTestAccess::SanitizeDisplayName(Padded), FString::ChrN(64, TEXT('C')));
	}

	// Surrogate pairs. Where TCHAR is 16 bits an emoji is TWO FString characters, and half of one
	// is not a character — the platform validates nothing, so a split pair would be stored.
	// (Where TCHAR is 32 bits there are no surrogates and these simply cut on a code point.)
	{
		const FString Emoji = TEXT("\U0001F3AE");
		if (Emoji.Len() == 2)
		{
			// 63 filler + the 2-unit emoji = 65. The naive cut at 64 would keep the high surrogate
			// and drop its partner.
			AddExpectedError(TEXT("display name truncated"), EAutomationExpectedMessageFlags::Contains, 0);
			const FString Result = FPlayServAuthTestAccess::SanitizeDisplayName(FString::ChrN(63, TEXT('D')) + Emoji);
			TestEqual(TEXT("the cut backs off to 63 rather than splitting the pair"), Result.Len(), 63);
			TestFalse(TEXT("no orphaned high surrogate is left at the end"),
				Result.Len() > 0 && StringConv::IsHighSurrogate(static_cast<uint32>(Result[Result.Len() - 1])));

			// A pair that ENDS at 64 is whole and must be kept whole: 62 filler + emoji = 64.
			const FString Fits = FString::ChrN(62, TEXT('E')) + Emoji;
			TestEqual(TEXT("a pair ending exactly at the cap is kept"),
				FPlayServAuthTestAccess::SanitizeDisplayName(Fits), Fits);

			// The live probe name from the ticket: 7 UTF-16 units, one of them a pair. Nothing to do.
			const FString Probe = FString(TEXT("Фёдор ")) + Emoji;
			TestEqual(TEXT("a short unicode name with an emoji is untouched"),
				FPlayServAuthTestAccess::SanitizeDisplayName(Probe), Probe);
		}
		else
		{
			AddInfo(TEXT("TCHAR is not 16-bit here — no surrogate pairs exist, so the boundary case cannot arise"));
		}
	}

	// Truncation can expose whitespace that was interior before the cut. The platform preserves
	// it, so a name must not be allowed to end in a space the caller never wrote.
	{
		AddExpectedError(TEXT("display name truncated"), EAutomationExpectedMessageFlags::Contains, 0);
		const FString Result = FPlayServAuthTestAccess::SanitizeDisplayName(FString::ChrN(63, TEXT('F')) + TEXT(" tail"));
		TestEqual(TEXT("a space exposed by the cut is trimmed away"), Result, FString::ChrN(63, TEXT('F')));
	}

	return true;
}

// ---------------------------------------------------------------------------
// AnonRequestShape — the anonymous body, with and without a name (PSV-2646).
//
// The no-name overload must still post a literal `{}`: that is what every existing call site
// sends today, and the named overload exists precisely so those sites do not change. The named
// one must add exactly ONE field, spelled snake_case — `displayName` and `name` are both
// accepted by the platform and both write nothing, so the spelling is the whole assertion.
// Synchronous, no network.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthV2AnonRequestShapeTest,
	"PlayServ.Auth.V2.AnonRequestShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthV2AnonRequestShapeTest::RunTest(const FString& Parameters)
{
	{
		TSharedPtr<FJsonObject> Body = FPlayServAuthTestAccess::BuildAnonLoginBody(TEXT(""));
		if (!Body.IsValid())
		{
			AddError(TEXT("BuildAnonLoginBody returned null"));
			return true;
		}
		TestEqual(TEXT("the unnamed guest still posts an EMPTY object"), Body->Values.Num(), 0);
	}

	{
		TSharedPtr<FJsonObject> Body = FPlayServAuthTestAccess::BuildAnonLoginBody(TEXT("Guest Jane"));
		TestEqual(TEXT("a named guest posts EXACTLY one field"), Body->Values.Num(), 1);
		TestEqual(TEXT("and it is display_name, snake_case"),
			Body->GetStringField(TEXT("display_name")), FString(TEXT("Guest Jane")));
	}

	{
		// Sanitising is inside the builder, not at the call site: a whitespace-only name would
		// otherwise be posted, accepted, and stored as nothing.
		TSharedPtr<FJsonObject> Body = FPlayServAuthTestAccess::BuildAnonLoginBody(TEXT("   "));
		TestEqual(TEXT("a whitespace-only name posts an empty object, not display_name:\"\""), Body->Values.Num(), 0);
	}

	{
		TSharedPtr<FJsonObject> Body = FPlayServAuthTestAccess::BuildAnonLoginBody(TEXT("  Padded Jane  "));
		TestEqual(TEXT("the name is trimmed on the way out"),
			Body->GetStringField(TEXT("display_name")), FString(TEXT("Padded Jane")));
	}

	return true;
}

// ---------------------------------------------------------------------------
// RefreshFailureClassification — "the platform refused us" vs "we never reached it".
//
// PSV-2644 defect 2. The failure branch used to fire on !bSuccess and clear the session, on the
// claim that "the presented token has been consumed either way" — which is false when the
// connection never opened. The host's handler then erases a refresh token the platform still
// honours, so a Wi-Fi blip costs a player their identity permanently.
//
// This is the decision that did not exist before, which makes it the one a regression undoes.
// Synchronous, no network — the point is to cover EVERY error code, including ones a live test
// cannot produce on demand.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthV2RefreshFailureClassificationTest,
	"PlayServ.Auth.V2.RefreshFailureClassification",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthV2RefreshFailureClassificationTest::RunTest(const FString& Parameters)
{
	auto IsTerminal = [](EPlayServErrorCode Code)
	{
		return FPlayServAuthTestAccess::IsTerminalRefreshFailure(FPlayServError::Make(Code, TEXT("synthetic")));
	};

	// No answer arrived: the platform never saw the token, so it is still the live credential.
	// Keeping the session is the fix — anything that broadcasts here deletes a working identity.
	TestFalse(TEXT("NetworkUnreachable is NOT terminal — the request never reached the platform"),
		IsTerminal(EPlayServErrorCode::NetworkUnreachable));
	TestFalse(TEXT("Timeout is NOT terminal — no answer is not a refusal"),
		IsTerminal(EPlayServErrorCode::Timeout));

	// The platform answered, so the request was processed and the presented token is spent.
	TestTrue(TEXT("Unauthorized is terminal — 401 means the family is revoked"),
		IsTerminal(EPlayServErrorCode::Unauthorized));
	TestTrue(TEXT("Forbidden is terminal"), IsTerminal(EPlayServErrorCode::Forbidden));
	TestTrue(TEXT("NotFound is terminal"), IsTerminal(EPlayServErrorCode::NotFound));
	TestTrue(TEXT("Conflict is terminal"), IsTerminal(EPlayServErrorCode::Conflict));
	TestTrue(TEXT("PreconditionFailed is terminal"), IsTerminal(EPlayServErrorCode::PreconditionFailed));
	TestTrue(TEXT("ValidationFailed is terminal"), IsTerminal(EPlayServErrorCode::ValidationFailed));
	TestTrue(TEXT("ContractMismatch is terminal"), IsTerminal(EPlayServErrorCode::ContractMismatch));

	// Unknown covers a 5xx the platform DID answer with, so it stays terminal: the token may have
	// been consumed, and re-presenting a consumed token is what revokes a live family.
	TestTrue(TEXT("Unknown is terminal — a 5xx is still an answer"), IsTerminal(EPlayServErrorCode::Unknown));

	// A request the transport never dispatched is classified at the HTTP layer as
	// NetworkUnreachable for exactly this reason — it must not read as "the platform answered".
	TestFalse(TEXT("the not-dispatched case lands on the non-terminal side"),
		IsTerminal(EPlayServErrorCode::NetworkUnreachable));

	return true;
}

// ---------------------------------------------------------------------------
// RefreshTokenLoginIsSingleFlighted — PSV-2644 defect 1.
//
// LoginWithRefreshToken spoke the rotating-credential endpoint with no guard, while
// RefreshSession held one of its own. Two overlapping calls present the same rt_, the platform
// reads the second as rotation reuse and revokes the WHOLE family — and the resulting 401 was
// then swallowed by the SessionGeneration check the first call had just bumped, so no callback
// fired and nothing surfaced. The player sat in-game on a session already dead server-side.
//
// The regression guard for the whole family-revocation failure: the second call must be refused
// LOCALLY. Synchronous refusal is the proof that no second request went out — a dispatched
// request cannot call back on the same stack.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthV2RefreshTokenLoginIsSingleFlightedTest,
	"PlayServ.Auth.V2.RefreshTokenLoginIsSingleFlighted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthV2RefreshTokenLoginIsSingleFlightedTest::RunTest(const FString& Parameters)
{
	// A syntactically plausible token that the platform will refuse. It has to be non-empty to get
	// past the fail-fast guard and actually occupy the in-flight slot; whether it is accepted is
	// irrelevant, because the assertion is about the SECOND call never being sent.
	const FString Token = TEXT("rt_single_flight_probe_0000000000000000000");

	TSharedPtr<bool> bFirstFired = MakeShared<bool>(false);
	TSharedPtr<bool> bSecondFired = MakeShared<bool>(false);
	TSharedPtr<bool> bSecondOk = MakeShared<bool>(true);
	TSharedPtr<FString> SecondError = MakeShared<FString>();
	TSharedPtr<FPlayServError> SecondErrorValue = MakeShared<FPlayServError>();
	TSharedPtr<FString> SecondPlayerId = MakeShared<FString>(TEXT("unset"));
	TSharedPtr<bool> bInFlightBetween = MakeShared<bool>(false);

	// Both calls on ONE stack, which is how a double-clicked LOGIN button reaches this.
	TSharedPtr<bool> bIssued = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("TwoLoginsBackToBack"),
		[Token, bFirstFired, bSecondFired, bSecondOk, SecondError, SecondErrorValue, SecondPlayerId, bInFlightBetween, bIssued]()
	{
		PlayServ::Auth::LoginWithRefreshToken(Token, FPlayServAuthCallback::CreateLambda(
			[bFirstFired](bool, const FString&, const FPlayServError&) { *bFirstFired = true; }));

		if (UPlayServAuth* Auth = AuthModule())
		{
			*bInFlightBetween = FPlayServAuthTestAccess::IsRefreshInFlight(Auth);
		}

		PlayServ::Auth::LoginWithRefreshToken(Token, FPlayServAuthCallback::CreateLambda(
			[bSecondFired, bSecondOk, SecondError, SecondErrorValue, SecondPlayerId](bool bOk, const FString& PlayerId, const FPlayServError& Error)
			{
				*bSecondOk = bOk;
				*SecondError = Error.Message;
				*SecondErrorValue = Error;
				*SecondPlayerId = PlayerId;
				*bSecondFired = true;
			}));

		*bIssued = true;
	}, bIssued));

	// Asserted in the same frame the calls were made: the first is still in flight here, so a
	// second callback can only have come from a local refusal.
	AddCommand(new FPlayServAssertStep(this,
		[bFirstFired, bSecondFired, bSecondOk, SecondError, SecondErrorValue, SecondPlayerId, bInFlightBetween](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("the first call took the in-flight slot"), *bInFlightBetween);
		T->TestFalse(TEXT("the first call has NOT come back yet — the overlap is real"), *bFirstFired);

		T->TestTrue(TEXT("the second call was refused SYNCHRONOUSLY — so no second request was sent"), *bSecondFired);
		T->TestFalse(TEXT("the second call reports failure"), *bSecondOk);
		T->TestTrue(TEXT("no player id is reported on the refusal"), SecondPlayerId->IsEmpty());
		T->TestTrue(FString::Printf(TEXT("the error says WHY, rather than dropping the call silently (got: %s)"), **SecondError),
			SecondError->Contains(TEXT("in flight")));

		// The code, not just the message — and this is the assertion that matters most.
		//
		// A local refusal sent NOTHING, so the caller's stored token is still the live credential.
		// The SDK teaches callers a binary rule (see IsTerminalRefreshFailure, and BlobEater's
		// OnResumeComplete which implements it): anything not on the "never reached the platform"
		// side means the platform answered and the token is dead, so discard it. Classifying this
		// refusal on the wrong side of that line makes a caller erase a perfectly valid credential
		// — the exact data loss PSV-2644 exists to prevent, reintroduced by the guard meant to fix it.
		T->TestFalse(TEXT("the refusal is NON-terminal — nothing was sent, so the stored token is still live"),
			FPlayServAuthTestAccess::IsTerminalRefreshFailure(*SecondErrorValue));
	}));

	// Let the first request settle so it does not land inside whatever test runs next.
	AddCommand(new FPlayServPollStep(this, TEXT("FirstCallSettles"),
		[bFirstFired]() { return *bFirstFired; }, 15.0f));

	AddCommand(new FPlayServAssertStep(this, [](FAutomationTestBase* T)
	{
		UPlayServAuth* Auth = AuthModule();
		if (Auth != nullptr)
		{
			T->TestFalse(TEXT("the in-flight slot is released once the request settles"),
				FPlayServAuthTestAccess::IsRefreshInFlight(Auth));
			T->TestFalse(TEXT("a refused token started no session"), Auth->IsLoggedIn());
		}
	}));

	return true;
}

// ---------------------------------------------------------------------------
// RotationHandlerOwnsItsToken — PSV-2644 defect 4.
//
// The hook handed out a `const FString&` aliasing the live RefreshToken member. The public docs
// invite a handler to call Logout when its own write fails; Logout reaches ClearSession, which
// empties that member, and the handler watched its own argument become an empty string halfway
// through — after which it would persist nothing, or persist garbage.
//
// Live, because the handler has to fire against a real session to reproduce it.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthV2RotationHandlerOwnsItsTokenTest,
	"PlayServ.Auth.V2.RotationHandlerOwnsItsToken",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthV2RotationHandlerOwnsItsTokenTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FString> SeenAtEntry = MakeShared<FString>();
	TSharedPtr<FString> SeenAfterReentry = MakeShared<FString>();
	TSharedPtr<bool> bHandlerRan = MakeShared<bool>(false);

	TSharedPtr<bool> bInstalled = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("InstallReentrantHandler"),
		[bInstalled, SeenAtEntry, SeenAfterReentry, bHandlerRan]()
	{
		PlayServ::Auth::SetRefreshTokenChangedHandler(FPlayServRefreshTokenChanged::CreateLambda(
			[SeenAtEntry, SeenAfterReentry, bHandlerRan](const FString& RefreshToken)
			{
				if (*bHandlerRan)
				{
					return;
				}
				*bHandlerRan = true;
				*SeenAtEntry = RefreshToken;

				// Exactly what the customer reference tells a host to do when its own write fails.
				// This re-enters the SDK and empties the member the parameter used to alias.
				PlayServ::Auth::Logout(FPlayServSimpleCallback::CreateLambda([](bool, const FPlayServError&) {}));

				// Read the SAME parameter again, after the re-entry.
				*SeenAfterReentry = RefreshToken;
			}));
		*bInstalled = true;
	}, bInstalled));

	TSharedPtr<FString> PlayerId = MakeShared<FString>();
	AddAnonLoginStep(this, PlayerId);

	AddCommand(new FPlayServAssertStep(this, [SeenAtEntry, SeenAfterReentry, bHandlerRan](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("the handler ran"), *bHandlerRan);
		T->TestTrue(TEXT("it was handed a non-empty token"), !SeenAtEntry->IsEmpty());
		T->TestFalse(TEXT("the argument did NOT empty itself when the handler re-entered the SDK"),
			SeenAfterReentry->IsEmpty());

		// Compared as a bool on purpose: TestEqual prints both values on failure, and these are
		// live refresh tokens. A failing assertion must not be the thing that leaks the credential
		// into a log somebody pastes into a ticket.
		T->TestTrue(TEXT("it is the same value before and after the re-entry"),
			*SeenAfterReentry == *SeenAtEntry);
	}));

	// Leave no handler installed for whatever runs next in this process.
	TSharedPtr<bool> bCleared = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("ClearHandler"), [bCleared]()
	{
		PlayServ::Auth::SetRefreshTokenChangedHandler(FPlayServRefreshTokenChanged());
		*bCleared = true;
	}, bCleared));

	return true;
}

// ---------------------------------------------------------------------------
// NamedAnonLoginLandsOnThePlatform — the end-to-end pin for PSV-2646.
//
// Anonymous login WITH a name, then read the player record back over the raw wire and assert the
// name is there. This is the only thing pinning the snake_case spelling from our side: nothing in
// the backend suite asserts the JSON key, and the platform answers 200 and writes nothing for
// `displayName` or `name` — so a typo in the SDK is invisible to every other test we have.
//
// Deliberately NOT faked at FPlayServHttp: the defect class this guards (a field the platform
// silently ignores) lives in the real request, and a fake would assert our own spelling against
// itself. The read-back uses the raw wire helper rather than an SDK call, so the assertion does
// not depend on the same code it is checking.
//
// Every run mints a FRESH player, which is required: the platform fills a name only when the
// login creates the player, and never retroactively fixes an existing one.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthV2NamedAnonLoginLandsOnThePlatformTest,
	"PlayServ.Auth.V2.NamedAnonLoginLandsOnThePlatform",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthV2NamedAnonLoginLandsOnThePlatformTest::RunTest(const FString& Parameters)
{
	// Unique per run so a stale record cannot make a broken build look green. Unicode and an emoji
	// on purpose — the SDK truncates on a UTF-16 boundary, and this proves a whole pair survives
	// the round trip rather than only ASCII doing so.
	const FString SentName = FString::Printf(TEXT("Фёдор \U0001F3AE %s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8));

	TSharedPtr<FString> PlayerId = MakeShared<FString>();
	TSharedPtr<bool> bLoginOk = MakeShared<bool>(false);
	TSharedPtr<FString> LoginError = MakeShared<FString>();

	TSharedPtr<bool> bLoggedIn = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("NamedAnonLogin"),
		[SentName, PlayerId, bLoginOk, LoginError, bLoggedIn]()
	{
		PlayServ::Auth::LoginAnonymous(SentName, FPlayServAuthCallback::CreateLambda(
			[PlayerId, bLoginOk, LoginError, bLoggedIn](bool bSuccess, const FString& InPlayerId, const FPlayServError& Error)
			{
				*bLoginOk = bSuccess;
				*PlayerId = InPlayerId;
				*LoginError = Error.Message;
				*bLoggedIn = true;
			}));
	}, bLoggedIn, 10.0f));

	AddCommand(new FPlayServAssertStep(this, [PlayerId, bLoginOk, LoginError](FAutomationTestBase* T)
	{
		T->TestTrue(FString::Printf(TEXT("named anonymous login succeeded (%s)"), **LoginError), *bLoginOk);
		T->TestTrue(TEXT("a fresh plr_* was minted"), PlayerId->StartsWith(TEXT("plr_")));
	}));

	// GET /data/players/{playerId} with pk_ + this player's own bearer. The path is only known
	// once the login has landed, so the request is fired from inside the step rather than built
	// at construction time.
	PlayServWireTest::FWireResultPtr Record = MakeShared<PlayServWireTest::FWireResult>();
	TSharedPtr<bool> bFired = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("ReadPlayerRecord"), [PlayerId, Record, bFired]()
	{
		PlayServWireTest::Fire(TEXT("GET"), FString::Printf(TEXT("/data/players/%s"), **PlayerId),
			TEXT("player"), nullptr, Record);
		*bFired = true;
	}, bFired));
	AddCommand(new FPlayServPollStep(this, TEXT("RecordArrives"),
		[Record]() { return Record->bCompleted; }, 15.0f));

	AddCommand(new FPlayServAssertStep(this, [Record, SentName](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("the player record reads back"), Record->Status, 200);
		if (!Record->Json.IsValid())
		{
			T->AddError(FString::Printf(TEXT("GET /data/players returned no JSON (status %d)"), Record->Status));
			return;
		}

		FString StoredName;
		T->TestTrue(TEXT("the record carries a name field"), Record->Json->TryGetStringField(TEXT("name"), StoredName));

		// The assertion the whole test exists for. An empty name here means the SDK's key never
		// matched the platform's — which is exactly what a 200 response will not tell you.
		T->TestFalse(TEXT("the name is not blank — display_name reached the platform"), StoredName.IsEmpty());
		T->TestEqual(TEXT("the stored name is what the SDK sent, unicode and emoji intact"), StoredName, SentName);
	}));

	AddV2LogoutStep(this);
	return true;
}

#endif // !UE_BUILD_SHIPPING
