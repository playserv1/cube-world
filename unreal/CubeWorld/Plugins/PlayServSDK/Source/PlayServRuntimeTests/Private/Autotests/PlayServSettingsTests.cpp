#include "Core/PlayServSettings.h"
#include "PlayServTestAccess.h"
#include "Misc/AutomationTest.h"
#include "Misc/CoreMisc.h"

#if !UE_BUILD_SHIPPING

// ---------------------------------------------------------------------------
// PlayServ.Core.Settings.ServerKeyPrecedence
//
// The server key resolves command line > PLAYSERV_SERVER_KEY > ini. The rule is a pure private
// function, reached through FPlayServSettingsTestAccess (it is not customer API), so it can be
// pinned offline without touching FCommandLine or the process environment.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServSettingsServerKeyPrecedenceTest,
	"PlayServ.Core.Settings.ServerKeyPrecedence",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServSettingsServerKeyPrecedenceTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("command line wins over environment and ini"),
		FPlayServSettingsTestAccess::ResolveServerKey(TEXT("sk_cmd"), TEXT("sk_env"), TEXT("sk_ini")), FString(TEXT("sk_cmd")));
	TestEqual(TEXT("environment wins over ini"),
		FPlayServSettingsTestAccess::ResolveServerKey(TEXT(""), TEXT("sk_env"), TEXT("sk_ini")), FString(TEXT("sk_env")));
	TestEqual(TEXT("ini is the default"),
		FPlayServSettingsTestAccess::ResolveServerKey(TEXT(""), TEXT(""), TEXT("sk_ini")), FString(TEXT("sk_ini")));
	TestTrue(TEXT("nothing configured resolves to empty"),
		FPlayServSettingsTestAccess::ResolveServerKey(TEXT(""), TEXT(""), TEXT("")).IsEmpty());
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Core.Settings.LaunchListenPortIsDigitsOnly  (PSV-2712)
//
// PLAYSERV_ROOM_LISTEN_PORT becomes the port a hosted room advertises, so anything that is not
// plainly a port reads as none — a room refused `launch_incomplete` beats a room advertising a
// port nobody listens on.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServSettingsLaunchPortTest,
	"PlayServ.Core.Settings.LaunchListenPortIsDigitsOnly",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServSettingsLaunchPortTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("a port"), FPlayServSettingsTestAccess::ParseLaunchPort(TEXT("7777")), 7777);
	TestEqual(TEXT("whitespace and quotes are trimmed"), FPlayServSettingsTestAccess::ParseLaunchPort(TEXT(" \"7778\" ")), 7778);
	TestEqual(TEXT("the top of the range"), FPlayServSettingsTestAccess::ParseLaunchPort(TEXT("65535")), 65535);
	TestEqual(TEXT("absent"), FPlayServSettingsTestAccess::ParseLaunchPort(TEXT("")), 0);
	TestEqual(TEXT("zero is no port"), FPlayServSettingsTestAccess::ParseLaunchPort(TEXT("0")), 0);
	TestEqual(TEXT("past the range"), FPlayServSettingsTestAccess::ParseLaunchPort(TEXT("65536")), 0);
	TestEqual(TEXT("signed"), FPlayServSettingsTestAccess::ParseLaunchPort(TEXT("-7777")), 0);
	TestEqual(TEXT("fractional"), FPlayServSettingsTestAccess::ParseLaunchPort(TEXT("7777.5")), 0);
	TestEqual(TEXT("not a number"), FPlayServSettingsTestAccess::ParseLaunchPort(TEXT("seven")), 0);
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Core.Settings.ConfigDomainIsGame
//
// The settings live in the project's DefaultGame.ini (Config=Game), and the dedicated server's
// key sits in Config/DedicatedServerGame.ini — a file the engine loads only when
// IsRunningDedicatedServer(). So in this (non-DS) test process no server key may be loaded.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServSettingsConfigDomainTest,
	"PlayServ.Core.Settings.ConfigDomainIsGame",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServSettingsConfigDomainTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("UPlayServSettings reads DefaultGame.ini (Config=Game)"),
		UPlayServSettings::StaticClass()->ClassConfigName.ToString(), FString(TEXT("Game")));
	TestTrue(TEXT("a non-dedicated-server process loads no ServerKey (DedicatedServerGame.ini is outside its hierarchy)"),
		IsRunningDedicatedServer() || UPlayServSettings::GetServerKey().IsEmpty());
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Core.Settings.RoomDefaultSlugReachesTheClient  (PSV-2645)
//
// The slug-less Browse / JoinRoom read RoomDefaultSlug, so a CLIENT has to be able to see it.
// It used to sit in Config/DedicatedServerGame.ini, which the engine loads only under
// IsRunningDedicatedServer() — so in this (non-DS) process it read back empty, and a client
// would have browsed a room type nobody hosts. It lives in DefaultGame.ini now.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServSettingsRoomDefaultSlugReachesClientTest,
	"PlayServ.Core.Settings.RoomDefaultSlugReachesTheClient",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServSettingsRoomDefaultSlugReachesClientTest::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("a non-dedicated-server process reads the room type slug from DefaultGame.ini"),
		UPlayServSettings::GetRoomDefaultSlug().IsEmpty());
	return true;
}

#endif
