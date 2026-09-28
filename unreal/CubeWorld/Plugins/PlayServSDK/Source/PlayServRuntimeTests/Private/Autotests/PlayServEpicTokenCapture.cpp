#include "PlayServTestCommon.h"
#include "OnlineSubsystem.h"
#include "Interfaces/OnlineIdentityInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#if !UE_BUILD_SHIPPING

// ---------------------------------------------------------------------------
// PlayServ.Auth.CaptureEpicToken
//
// Dev-only utility (NOT a behavioural test): performs an EOS OnlineSubsystem
// login and writes the resulting Epic Account access token to a local file.
// This is the exact value LoginExternal's FPlayServExternalCredential carries
// when Type == EpicAccessToken (the SDK only transports it; the caller obtains
// it — which is what this utility does, playing the caller's role).
//
// Default login type is "persistentauth" — fully headless, but only succeeds if
// a prior interactive Epic sign-in left a cached refresh token on this machine.
// If it fails, set up the EOS DevAuthTool and re-run with the developer type
// (no rebuild needed — see overrides below).
//
// Command-line overrides (all optional):
//   -EpicLoginType=persistentauth|developer|accountportal  (default persistentauth)
//   -EpicDevAuthHost=localhost:6300                         (developer type only)
//   -EpicDevAuthCred=<credential name>                      (developer type only)
//   -EpicTokenOutFile=<abs path>                            (default <Saved>/EpicAccessToken.txt)
//
// The token is a bearer credential: it is written to a gitignored file and only
// its length + a short prefix are logged — never the full value (mirrors the
// SDK's no-tokens-in-logs policy).
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServAuthCaptureEpicTokenTest,
	"PlayServ.Auth.CaptureEpicToken",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServAuthCaptureEpicTokenTest::RunTest(const FString& Parameters)
{
	// --- Resolve command-line overrides (synchronous, before latent steps) ---
	FString LoginType = TEXT("persistentauth");
	FParse::Value(FCommandLine::Get(), TEXT("EpicLoginType="), LoginType);

	FString DevAuthHost = TEXT("localhost:6300");
	FParse::Value(FCommandLine::Get(), TEXT("EpicDevAuthHost="), DevAuthHost);

	FString DevAuthCred;
	FParse::Value(FCommandLine::Get(), TEXT("EpicDevAuthCred="), DevAuthCred);

	FString OutFile = FPaths::ProjectSavedDir() / TEXT("EpicAccessToken.txt");
	FParse::Value(FCommandLine::Get(), TEXT("EpicTokenOutFile="), OutFile);
	OutFile = FPaths::ConvertRelativePathToFull(OutFile);

	// Credential Id/Token differ per login type:
	//   persistentauth / accountportal → Id="",          Token=""
	//   developer                      → Id=host:port,   Token=credential name
	const bool bIsDeveloper = LoginType.Equals(TEXT("developer"), ESearchCase::IgnoreCase);
	const FString CredId = bIsDeveloper ? DevAuthHost : FString();
	const FString CredToken = bIsDeveloper ? DevAuthCred : FString();

	AddInfo(FString::Printf(TEXT("EOS login type '%s'%s — output: %s"),
		*LoginType,
		bIsDeveloper ? *FString::Printf(TEXT(" (host=%s, cred=%s)"), *DevAuthHost, *DevAuthCred) : TEXT(""),
		*OutFile));

	// Shared state across latent steps
	TSharedPtr<bool> bLoginDone = MakeShared<bool>(false);
	TSharedPtr<bool> bLoginOk = MakeShared<bool>(false);
	TSharedPtr<FString> LoginError = MakeShared<FString>();
	TSharedPtr<FDelegateHandle> LoginHandle = MakeShared<FDelegateHandle>();

	// --- Step 1: EOS OnlineSubsystem login ---
	// Re-entrancy note: EOS can fire OnLoginComplete *synchronously* from inside Login()
	// (notably on persistent-auth failure). The callback only sets shared flags — it never
	// calls AddLambda/Login/Remove — and delegate cleanup is deferred to Step 2 so we never
	// mutate the multicast mid-broadcast (see UBlobGameInstance::LoginEOS for that crash).
	AddCommand(new FPlayServAsyncStep(this, TEXT("EOSLogin"),
		[bLoginDone, bLoginOk, LoginError, LoginHandle, LoginType, CredId, CredToken]()
		{
			IOnlineSubsystem* OnlineSub = IOnlineSubsystem::Get(TEXT("EOS"));
			if (!OnlineSub)
			{
				*LoginError = TEXT("OnlineSubsystem 'EOS' not available (plugin not loaded?)");
				*bLoginDone = true;
				return;
			}

			IOnlineIdentityPtr Identity = OnlineSub->GetIdentityInterface();
			if (!Identity.IsValid())
			{
				*LoginError = TEXT("EOS Identity interface not available");
				*bLoginDone = true;
				return;
			}

			// Already signed in (e.g. earlier step in the same process) — reuse it.
			if (Identity->GetLoginStatus(0) == ELoginStatus::LoggedIn)
			{
				*bLoginOk = true;
				*bLoginDone = true;
				return;
			}

			*LoginHandle = Identity->AddOnLoginCompleteDelegate_Handle(0,
				FOnLoginCompleteDelegate::CreateLambda(
					[bLoginDone, bLoginOk, LoginError](int32, bool bWasSuccessful, const FUniqueNetId&, const FString& Error)
					{
						*bLoginOk = bWasSuccessful;
						*LoginError = Error;
						*bLoginDone = true;
					}));

			Identity->Login(0, FOnlineAccountCredentials(LoginType, CredId, CredToken));
		},
		bLoginDone, 120.0f));

	// --- Step 2: capture token, write file, clear delegate ---
	AddCommand(new FPlayServAssertStep(this,
		[bLoginOk, LoginError, LoginHandle, OutFile, LoginType](FAutomationTestBase* T)
		{
			// Safe to touch the delegate now — well past Login()'s synchronous broadcast.
			if (IOnlineSubsystem* OnlineSub = IOnlineSubsystem::Get(TEXT("EOS")))
			{
				if (IOnlineIdentityPtr Identity = OnlineSub->GetIdentityInterface())
				{
					if (LoginHandle->IsValid())
					{
						Identity->ClearOnLoginCompleteDelegate_Handle(0, *LoginHandle);
					}
				}
			}

			if (!*bLoginOk)
			{
				T->AddError(FString::Printf(TEXT("EOS '%s' login failed: %s"),
					*LoginType, LoginError->IsEmpty() ? TEXT("(no error string)") : **LoginError));
				if (LoginType.Equals(TEXT("persistentauth"), ESearchCase::IgnoreCase))
				{
					T->AddError(TEXT("No cached persistent credential on this machine. Set up the EOS ")
						TEXT("DevAuthTool (Engine/Source/ThirdParty/EOSSDK/SDK/Tools/EOS_DevAuthTool-win32-x64-1.2.1.zip), ")
						TEXT("sign in once, then re-run with: ")
						TEXT("-EpicLoginType=developer -EpicDevAuthHost=localhost:6300 -EpicDevAuthCred=<name>"));
				}
				return;
			}

			IOnlineSubsystem* OnlineSub = IOnlineSubsystem::Get(TEXT("EOS"));
			IOnlineIdentityPtr Identity = OnlineSub ? OnlineSub->GetIdentityInterface() : nullptr;
			const FString Token = Identity.IsValid() ? Identity->GetAuthToken(0) : FString();

			T->TestFalse(TEXT("Epic access token should not be empty"), Token.IsEmpty());
			if (Token.IsEmpty())
			{
				T->AddError(TEXT("EOS login succeeded but GetAuthToken(0) was empty. ")
					TEXT("Confirm bUseEAS=true so GetAuthToken returns the Epic Account access token."));
				return;
			}

			if (!FFileHelper::SaveStringToFile(Token, *OutFile))
			{
				T->AddError(FString::Printf(TEXT("Failed to write token to %s"), *OutFile));
				return;
			}

			// Never log the full bearer token — length + short prefix only.
			T->AddInfo(FString::Printf(TEXT("Epic access token captured: %d chars, prefix '%s...' — written to %s"),
				Token.Len(), *Token.Left(6), *OutFile));
		}));

	return true;
}

#endif // !UE_BUILD_SHIPPING
