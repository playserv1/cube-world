#include "PlayServTestCommon.h"
#include "PlayServTestAccess.h"
#include "PlayServ.h"

#if !UE_BUILD_SHIPPING

// ---------------------------------------------------------------------------
// PlayServ.Auth.Launcher.* — Epic Games Launcher entry detection.
//
// The requirement under test: "launcher entry is auto-detected from the command line;
// launcher code and EOS are mutually exclusive when both are configured".
//
// These tests are synchronous and offline — no network, no launcher, no Epic entitlement.
// That is deliberate and is why the detection is a pure function over a string: a REAL
// launcher entry cannot be reproduced on a dev box, since the one-time exchange code is
// minted by the Epic Games Launcher against a real Epic Games Store entitlement. The live
// round-trip therefore stays a manual step (as it does for native Epic login),
// and everything that CAN be decided without it is decided here.
//
// The command line the launcher hands a game (Epic's Auth Interface documentation):
//   -AUTH_LOGIN=unused -AUTH_PASSWORD=<exchange code> -AUTH_TYPE=exchangecode
//   -epicapp=<id> -epicenv=Prod -EpicPortal -epicusername=<name> -epicuserid=<id> -epiclocale=<loc>
// ---------------------------------------------------------------------------

namespace
{
	/** A full, realistically-ordered launcher command line. */
	const TCHAR* const LauncherCommandLine =
		TEXT("-AUTH_LOGIN=unused -AUTH_PASSWORD=6f0e1c9a4b2d47f8a1c3e5d7b9f0a2c4 -AUTH_TYPE=exchangecode ")
		TEXT("-epicapp=PlayServTitle -epicenv=Prod -EpicPortal ")
		TEXT("-epicusername=\"Player One\" -epicuserid=0123456789abcdef0123456789abcdef -epiclocale=en-US");

	const TCHAR* const ExpectedExchangeCode = TEXT("6f0e1c9a4b2d47f8a1c3e5d7b9f0a2c4");
}

// ---------------------------------------------------------------------------
// The happy path: a real launcher command line yields the exchange code verbatim.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthLauncherDetectsExchangeCodeTest,
	"PlayServ.Auth.Launcher.DetectsExchangeCode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthLauncherDetectsExchangeCodeTest::RunTest(const FString& Parameters)
{
	FString Code;
	const bool bDetected = FPlayServAuthTestAccess::ParseLauncherExchangeCode(LauncherCommandLine, Code);

	TestTrue(TEXT("a launcher command line is detected"), bDetected);
	TestEqual(TEXT("the exchange code is extracted verbatim from -AUTH_PASSWORD"), Code, FString(ExpectedExchangeCode));
	return true;
}

// ---------------------------------------------------------------------------
// An ordinary launch is not a launcher launch. Guards against the detection firing on the
// SDK's own overrides, which sit on the very same command line.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthLauncherIgnoresOrdinaryCommandLineTest,
	"PlayServ.Auth.Launcher.IgnoresOrdinaryCommandLine",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthLauncherIgnoresOrdinaryCommandLineTest::RunTest(const FString& Parameters)
{
	FString Code = TEXT("untouched");
	const bool bDetected = FPlayServAuthTestAccess::ParseLauncherExchangeCode(
		TEXT("-PlayServBaseURL=https://platform.example.invalid -PlayServClientKey=pk_example -game -log"), Code);

	TestFalse(TEXT("an ordinary command line is NOT a launcher entry"), bDetected);
	TestEqual(TEXT("the out parameter is left untouched on a negative"), Code, FString(TEXT("untouched")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthLauncherIgnoresEmptyCommandLineTest,
	"PlayServ.Auth.Launcher.IgnoresEmptyCommandLine",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthLauncherIgnoresEmptyCommandLineTest::RunTest(const FString& Parameters)
{
	FString Code;
	TestFalse(TEXT("an empty command line is not a launcher entry"),
		FPlayServAuthTestAccess::ParseLauncherExchangeCode(TEXT(""), Code));
	TestFalse(TEXT("a null command line is not a launcher entry (no crash)"),
		FPlayServAuthTestAccess::ParseLauncherExchangeCode(nullptr, Code));
	return true;
}

// ---------------------------------------------------------------------------
// THE mutual-exclusivity test, client side.
//
// -AUTH_PASSWORD is not an exchange-code field — it carries whatever credential -AUTH_TYPE
// names. Reading it without gating on the type would take a refresh token (or a literal
// password) and post it to the platform as mode:"launcher_exchange_code". The type gate is
// the only thing standing between those two readings, so it gets its own test.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthLauncherIgnoresOtherAuthTypesTest,
	"PlayServ.Auth.Launcher.IgnoresOtherAuthTypes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthLauncherIgnoresOtherAuthTypesTest::RunTest(const FString& Parameters)
{
	FString Code = TEXT("untouched");

	TestFalse(TEXT("-AUTH_TYPE=refreshtoken is not an exchange code, despite carrying a password"),
		FPlayServAuthTestAccess::ParseLauncherExchangeCode(
			TEXT("-AUTH_LOGIN=user -AUTH_PASSWORD=a-refresh-token-not-an-exchange-code -AUTH_TYPE=refreshtoken -EpicPortal"), Code));

	TestFalse(TEXT("a password with NO -AUTH_TYPE at all is not an exchange code"),
		FPlayServAuthTestAccess::ParseLauncherExchangeCode(
			TEXT("-AUTH_LOGIN=user -AUTH_PASSWORD=some-secret -EpicPortal"), Code));

	// -EpicPortal marks a launcher entry, not the presence of a code. A launcher entry with no
	// credential on it must read as "no launcher credential", not as an empty one.
	TestFalse(TEXT("-EpicPortal alone does not manufacture a credential"),
		FPlayServAuthTestAccess::ParseLauncherExchangeCode(
			TEXT("-EpicPortal -epicapp=PlayServTitle -epicenv=Prod"), Code));

	TestEqual(TEXT("the out parameter survives every negative untouched"), Code, FString(TEXT("untouched")));
	return true;
}

// ---------------------------------------------------------------------------
// An announced-but-absent code is a negative, never an empty credential.
//
// This is the sharp one. FParse::Value skips whitespace after the '=' before it starts
// reading (engine Parse.cpp, "Skip initial whitespace") and returns true for a zero-length
// result on purpose, so a bare `-AUTH_PASSWORD=` swallows the NEXT switch and hands it back
// as the value — non-empty, so an IsEmpty() check sails straight past it and the SDK posts an
// arbitrary command-line argument to the platform as an exchange code. Found by reading the
// engine source while wiring this up; ParseSwitchValue exists solely to close it.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthLauncherIgnoresEmptyExchangeCodeTest,
	"PlayServ.Auth.Launcher.IgnoresEmptyExchangeCode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthLauncherIgnoresEmptyExchangeCodeTest::RunTest(const FString& Parameters)
{
	FString Code = TEXT("untouched");

	TestFalse(TEXT("an empty -AUTH_PASSWORD followed by another switch is not a credential"),
		FPlayServAuthTestAccess::ParseLauncherExchangeCode(
			TEXT("-AUTH_LOGIN=unused -AUTH_PASSWORD= -AUTH_TYPE=exchangecode -EpicPortal"), Code));
	TestNotEqual(TEXT("the next switch was NOT swallowed as the exchange code"),
		Code, FString(TEXT("-AUTH_TYPE=exchangecode")));

	TestFalse(TEXT("an empty -AUTH_PASSWORD at the end of the line is not a credential"),
		FPlayServAuthTestAccess::ParseLauncherExchangeCode(
			TEXT("-AUTH_TYPE=exchangecode -EpicPortal -AUTH_PASSWORD="), Code));

	TestFalse(TEXT("an empty QUOTED -AUTH_PASSWORD is not a credential either"),
		FPlayServAuthTestAccess::ParseLauncherExchangeCode(
			TEXT("-AUTH_LOGIN=unused -AUTH_PASSWORD=\"\" -AUTH_TYPE=exchangecode -EpicPortal"), Code));

	TestFalse(TEXT("-AUTH_TYPE=exchangecode with no password at all is not a credential"),
		FPlayServAuthTestAccess::ParseLauncherExchangeCode(
			TEXT("-AUTH_TYPE=exchangecode -EpicPortal -epicapp=PlayServTitle"), Code));

	// The same hole on the type switch: a bare -AUTH_TYPE= must not swallow -AUTH_PASSWORD and
	// then fail the comparison for the wrong reason — it must read as "no type announced".
	TestFalse(TEXT("an empty -AUTH_TYPE is not an exchangecode announcement"),
		FPlayServAuthTestAccess::ParseLauncherExchangeCode(
			TEXT("-AUTH_TYPE= -AUTH_PASSWORD=6f0e1c9a4b2d47f8a1c3e5d7b9f0a2c4 -EpicPortal"), Code));

	TestEqual(TEXT("the out parameter survives every negative untouched"), Code, FString(TEXT("untouched")));
	return true;
}

// ---------------------------------------------------------------------------
// Quoting and casing. The launcher's own casing is fixed, but the value arrives through the
// same FParse path the SDK's other overrides use, which unquotes and matches case-insensitively
// — pinned so a future rewrite of the parse cannot quietly drop either.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthLauncherUnquotesAndIgnoresCaseTest,
	"PlayServ.Auth.Launcher.UnquotesAndIgnoresCase",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthLauncherUnquotesAndIgnoresCaseTest::RunTest(const FString& Parameters)
{
	FString Quoted;
	TestTrue(TEXT("a quoted exchange code is detected"),
		FPlayServAuthTestAccess::ParseLauncherExchangeCode(
			TEXT("-AUTH_LOGIN=unused -AUTH_PASSWORD=\"6f0e1c9a4b2d47f8a1c3e5d7b9f0a2c4\" -AUTH_TYPE=exchangecode"), Quoted));
	TestEqual(TEXT("surrounding quotes are stripped from the code"), Quoted, FString(ExpectedExchangeCode));

	FString MixedCase;
	TestTrue(TEXT("-auth_type=ExchangeCode is matched case-insensitively"),
		FPlayServAuthTestAccess::ParseLauncherExchangeCode(
			TEXT("-auth_login=unused -auth_password=6f0e1c9a4b2d47f8a1c3e5d7b9f0a2c4 -auth_type=ExchangeCode"), MixedCase));
	TestEqual(TEXT("the code survives case-insensitive matching intact"), MixedCase, FString(ExpectedExchangeCode));
	return true;
}

// ---------------------------------------------------------------------------
// The public entry over the REAL command line. An automation run is not a launcher entry, so
// this must report false — the one assertion in this file that exercises FCommandLine::Get()
// rather than a synthetic string, and the one that would catch a detection loose enough to
// fire on an ordinary editor launch.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthLauncherNoCredentialOnOrdinaryProcessTest,
	"PlayServ.Auth.Launcher.NoCredentialOnOrdinaryProcess",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthLauncherNoCredentialOnOrdinaryProcessTest::RunTest(const FString& Parameters)
{
	FPlayServExternalCredential Credential;
	const bool bDetected = PlayServ::Auth::TryGetLauncherCredential(Credential);

	TestFalse(TEXT("an automation run is not an Epic Games Launcher entry"), bDetected);
	TestEqual(TEXT("the credential is left untouched on a negative"),
		Credential.Type, EPlayServExternalAuthType::None);
	TestTrue(TEXT("no token was invented"), Credential.Token.IsEmpty());
	return true;
}

#endif // !UE_BUILD_SHIPPING
