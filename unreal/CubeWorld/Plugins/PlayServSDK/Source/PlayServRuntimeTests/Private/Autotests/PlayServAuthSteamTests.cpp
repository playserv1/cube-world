#include "PlayServTestCommon.h"
#include "PlayServTestAccess.h"
#include "PlayServ.h"

#if !UE_BUILD_SHIPPING

// ---------------------------------------------------------------------------
// PlayServ.Auth.Steam.* — Steam web-API ticket handling.
//
// Synchronous and offline. Minting a real ticket needs a running Steam client, a real app
// entitlement and Valve's web API on the far end, so — exactly as with the Epic launcher's
// exchange code — the parts that CAN be decided without any of that are decided here, and the
// live round trip stays a manual step.
//
// The two halves under test:
//
//   TryMakeSteamCredential  — bytes -> hex. Steamworks hands the ticket over as
//                             GetTicketForWebApiResponse_t::m_rgubTicket + m_cubTicket, and
//                             Valve's AuthenticateUserTicket wants hex. Nobody's login works
//                             if this is skipped or done by hand and got wrong.
//   IsWebApiTicketHex       — the pre-flight shape check LoginExternal runs on the Steam arm.
//
// What is deliberately NOT tested, because it cannot be: whether the ticket came from
// GetAuthTicketForWebApi rather than the deprecated GetAuthSessionTicket. Both are hex, so no
// client-side check can separate them — Valve's header says a session ticket "will fail" at
// AuthenticateUserTicket, and that failure is the only signal. It is documented on
// EPlayServExternalAuthType::SteamWebApiTicket instead of half-guarded here.
// ---------------------------------------------------------------------------

namespace
{
	/** Steamworks' own cap: GetTicketForWebApiResponse_t::k_nCubTicketMaxLength. */
	constexpr int32 SteamTicketMaxBytes = 2560;

	TArray<uint8> MakeTicketBytes(int32 Num)
	{
		TArray<uint8> Bytes;
		Bytes.Reserve(Num);
		for (int32 Index = 0; Index < Num; ++Index)
		{
			Bytes.Add(static_cast<uint8>(Index % 256));
		}
		return Bytes;
	}
}

// ---------------------------------------------------------------------------
// The happy path: raw ticket bytes become an uppercase-hex credential of the right type.
//
// Uppercase matters beyond taste — OnlineSubsystemSteam hex-encodes this same ticket with
// BytesToHex (OnlineAsyncTaskManagerSteam.cpp), so a game arriving through the OSS and a game
// arriving through raw Steamworks must put the identical string on the wire.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthSteamEncodesTicketBytesTest,
	"PlayServ.Auth.Steam.EncodesTicketBytes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthSteamEncodesTicketBytesTest::RunTest(const FString& Parameters)
{
	const uint8 Ticket[] = { 0x01, 0x00, 0xAB, 0xFF, 0x10 };

	FPlayServExternalCredential Credential;
	const bool bMade = PlayServ::Auth::TryMakeSteamCredential(MakeArrayView(Ticket, UE_ARRAY_COUNT(Ticket)), Credential);

	TestTrue(TEXT("a well-formed ticket encodes"), bMade);
	TestEqual(TEXT("the credential names the Steam arm"),
		static_cast<int32>(Credential.Type), static_cast<int32>(EPlayServExternalAuthType::SteamWebApiTicket));
	TestEqual(TEXT("bytes are hex-encoded uppercase, matching OnlineSubsystemSteam's BytesToHex"),
		Credential.Token, FString(TEXT("0100ABFF10")));
	return true;
}

// ---------------------------------------------------------------------------
// Rejections leave the out parameter untouched, so a caller that ignores the bool cannot end
// up posting a half-built credential. Same contract as TryGetLauncherCredential.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthSteamRejectsEmptyTicketTest,
	"PlayServ.Auth.Steam.RejectsEmptyTicket",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthSteamRejectsEmptyTicketTest::RunTest(const FString& Parameters)
{
	FPlayServExternalCredential Credential;
	Credential.Token = TEXT("untouched");

	const bool bMade = PlayServ::Auth::TryMakeSteamCredential(TConstArrayView<uint8>(), Credential);

	TestFalse(TEXT("an empty ticket is not a ticket"), bMade);
	TestEqual(TEXT("the out parameter is left untouched on a negative"), Credential.Token, FString(TEXT("untouched")));
	TestEqual(TEXT("the type is left untouched too"),
		static_cast<int32>(Credential.Type), static_cast<int32>(EPlayServExternalAuthType::None));
	return true;
}

// ---------------------------------------------------------------------------
// The size bound is Steam's, not ours, and it is checked on both sides so a fencepost slip
// cannot pass unnoticed: 2560 bytes is the largest ticket Steam mints and must encode; 2561
// can only be a length or offset mistake at the call site.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthSteamAcceptsMaximumTicketTest,
	"PlayServ.Auth.Steam.AcceptsMaximumTicket",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthSteamAcceptsMaximumTicketTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> Ticket = MakeTicketBytes(SteamTicketMaxBytes);

	FPlayServExternalCredential Credential;
	const bool bMade = PlayServ::Auth::TryMakeSteamCredential(Ticket, Credential);

	TestTrue(TEXT("the largest ticket Steam can mint still encodes"), bMade);
	TestEqual(TEXT("hex is exactly two characters per byte"), Credential.Token.Len(), SteamTicketMaxBytes * 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthSteamRejectsOversizeTicketTest,
	"PlayServ.Auth.Steam.RejectsOversizeTicket",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthSteamRejectsOversizeTicketTest::RunTest(const FString& Parameters)
{
	const TArray<uint8> Ticket = MakeTicketBytes(SteamTicketMaxBytes + 1);

	FPlayServExternalCredential Credential;
	Credential.Token = TEXT("untouched");
	const bool bMade = PlayServ::Auth::TryMakeSteamCredential(Ticket, Credential);

	TestFalse(TEXT("one byte over Steam's own cap cannot be a ticket"), bMade);
	TestEqual(TEXT("the out parameter is left untouched on a negative"), Credential.Token, FString(TEXT("untouched")));
	return true;
}

// ---------------------------------------------------------------------------
// The shape check. Case-insensitive on purpose: BytesToHex emits uppercase, but a game that
// lowercases the string somewhere in its own plumbing still holds a valid ticket — Valve's
// web API is not case-sensitive about it, so rejecting it here would be the SDK inventing a
// rule the wire does not have.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthSteamAcceptsHexTicketTest,
	"PlayServ.Auth.Steam.AcceptsHexTicket",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthSteamAcceptsHexTicketTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("uppercase hex is accepted"),
		FPlayServAuthTestAccess::IsWebApiTicketHex(TEXT("140000000100ABFF")));
	TestTrue(TEXT("lowercase hex is accepted — the wire is not case-sensitive"),
		FPlayServAuthTestAccess::IsWebApiTicketHex(TEXT("140000000100abff")));
	TestTrue(TEXT("mixed case is accepted"),
		FPlayServAuthTestAccess::IsWebApiTicketHex(TEXT("140000000100AbFf")));
	return true;
}

// ---------------------------------------------------------------------------
// An odd length cannot be a byte sequence. This is the truncation case: a copy that dropped a
// nibble still looks entirely hexish, and a check that only asked "are these hex characters?"
// would wave it through to a rejected login.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthSteamRejectsOddLengthTicketTest,
	"PlayServ.Auth.Steam.RejectsOddLengthTicket",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthSteamRejectsOddLengthTicketTest::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("a lone trailing nibble is a truncated ticket, not a short one"),
		FPlayServAuthTestAccess::IsWebApiTicketHex(TEXT("140000000100ABF")));
	TestFalse(TEXT("a single character is not a byte"),
		FPlayServAuthTestAccess::IsWebApiTicketHex(TEXT("A")));
	return true;
}

// ---------------------------------------------------------------------------
// The case this check exists for: the ticket arriving as raw bytes rather than hex, which is
// the shape Steamworks actually hands out. Without the guard it costs a network round trip to
// discover, and the error comes back from Valve rather than from the call site.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthSteamRejectsNonHexTicketTest,
	"PlayServ.Auth.Steam.RejectsNonHexTicket",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthSteamRejectsNonHexTicketTest::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("an empty string is not a ticket"),
		FPlayServAuthTestAccess::IsWebApiTicketHex(TEXT("")));
	TestFalse(TEXT("bytes reinterpreted as text are rejected"),
		FPlayServAuthTestAccess::IsWebApiTicketHex(TEXT("\x14\x00\x00\x00ticket")));
	TestFalse(TEXT("a base64-looking ticket is rejected — wrong encoding, right length parity"),
		FPlayServAuthTestAccess::IsWebApiTicketHex(TEXT("FAAAAAEAq/8=")));
	TestFalse(TEXT("an 0x prefix is not part of the wire format"),
		FPlayServAuthTestAccess::IsWebApiTicketHex(TEXT("0x0100ABFF")));
	TestFalse(TEXT("hex longer than Steam can mint is rejected"),
		FPlayServAuthTestAccess::IsWebApiTicketHex(FString::ChrN((SteamTicketMaxBytes * 2) + 2, TEXT('A'))));
	return true;
}

// ---------------------------------------------------------------------------
// The encoder and the validator are two halves of one contract, written at opposite ends of
// the module. This is the test that fails if either is changed alone — e.g. a switch to
// BytesToHexLower, or a bound tightened on one side only.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthSteamEncoderOutputPassesShapeCheckTest,
	"PlayServ.Auth.Steam.EncoderOutputPassesShapeCheck",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthSteamEncoderOutputPassesShapeCheckTest::RunTest(const FString& Parameters)
{
	for (const int32 Size : { 1, 2, 255, 256, SteamTicketMaxBytes })
	{
		FPlayServExternalCredential Credential;
		if (!PlayServ::Auth::TryMakeSteamCredential(MakeTicketBytes(Size), Credential))
		{
			AddError(FString::Printf(TEXT("TryMakeSteamCredential refused a %d-byte ticket"), Size));
			continue;
		}

		TestTrue(FString::Printf(TEXT("a %d-byte ticket encodes into something LoginExternal accepts"), Size),
			FPlayServAuthTestAccess::IsWebApiTicketHex(Credential.Token));
	}
	return true;
}

#endif // !UE_BUILD_SHIPPING
