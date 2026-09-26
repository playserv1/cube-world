#include "Auth/PlayServAuth.h"
#include "Auth/PlayServAuthResponse.h"
#include "Core/PlayServHttp.h"
#include "Core/PlayServJwt.h"
#include "Core/PlayServLog.h"
#include "Core/PlayServSubsystem.h"
#include "Data/PlayServData.h"
#include "Containers/StringConv.h"
#include "Containers/Ticker.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace
{
	const TCHAR* const DisplayNameField = TEXT("display_name");

	constexpr int32 MaxDisplayNameLength = 64;
}

void UPlayServAuth::Init(TSharedPtr<FPlayServHttp> InHttp)
{
	Http = InHttp;
}

void UPlayServAuth::Shutdown()
{
	StopRefreshTimer();
	ClearSession();
}

void UPlayServAuth::LoginAnonymous(FPlayServAuthCallback Callback)
{
	LoginAnonymous(FString(), MoveTemp(Callback));
}

void UPlayServAuth::LoginAnonymous(const FString& DisplayName, FPlayServAuthCallback Callback)
{
	UE_LOG(LogPlayServ, Display, TEXT("PlayServ: anonymous login attempt"));

	if (!Http.IsValid())
	{
		Callback.ExecuteIfBound(false, TEXT(""), FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("PlayServ HTTP not initialized")));
		return;
	}

	TWeakObjectPtr<UPlayServAuth> WeakThis(this);
	const uint32 ExpectedGeneration = SessionGeneration;

	Http->Request(EPlayServHttpVerb::Post, TEXT("/auth/players/anon"), BuildAnonLoginBody(DisplayName), FPlayServV2Callback::CreateLambda(
		[WeakThis, ExpectedGeneration, Callback](bool bSuccess, const FPlayServHttpResponse& Response, const FPlayServError& Error)
		{
			UPlayServAuth* Self = WeakThis.Get();
			if (!Self)
			{
				return;
			}

			if (Self->SessionGeneration != ExpectedGeneration)
			{
				return;
			}

			if (!bSuccess)
			{
				UE_LOG(LogPlayServ, Warning, TEXT("PlayServ: anonymous login failed: %s"), *Error.Message);
				Callback.ExecuteIfBound(false, TEXT(""), Error);
				return;
			}

			FPlayServAuthResponse AuthResponse = FPlayServAuthResponse::FromV2Bundle(Response.Json);
			if (AuthResponse.AccessToken.IsEmpty())
			{
				UE_LOG(LogPlayServ, Warning, TEXT("PlayServ: anonymous login failed: server returned no access token"));
				Callback.ExecuteIfBound(false, TEXT(""), FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("Login failed: server returned no access token")));
				return;
			}

			Self->StartSession(AuthResponse, EPlayServSessionType::Client);
			Self->StartRefreshTimer();

			if (UPlayServSubsystem* Subsystem = UPlayServSubsystem::Get())
			{
				Subsystem->GetData()->RunSchemaAdvisoryCheck();
			}

			UE_LOG(LogPlayServ, Display, TEXT("PlayServ: anonymous login success (PlayerId=%s)"), *AuthResponse.PlayerId);
			Callback.ExecuteIfBound(true, AuthResponse.PlayerId, FPlayServError::Success());
		}), {}, EPlayServPlayerBearer::Suppress);
}

void UPlayServAuth::LoginExternal(const FPlayServExternalCredential& Credential, FPlayServAuthCallback Callback)
{
	UE_LOG(LogPlayServ, Display, TEXT("PlayServ: external login attempt"));

	if (!Http.IsValid())
	{
		Callback.ExecuteIfBound(false, TEXT(""), FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("PlayServ HTTP not initialized")));
		return;
	}

	if (Credential.Type != EPlayServExternalAuthType::EpicAccessToken
		&& Credential.Type != EPlayServExternalAuthType::EpicLauncherExchangeCode
		&& Credential.Type != EPlayServExternalAuthType::SteamWebApiTicket)
	{
		UE_LOG(LogPlayServ, Warning, TEXT("PlayServ: external login rejected — auth type not yet supported"));
		Callback.ExecuteIfBound(false, TEXT(""), FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("PlayServ: external auth type not yet supported (only the Epic and Steam arms are available in this SDK version)")));
		return;
	}

	if (Credential.Token.IsEmpty())
	{
		UE_LOG(LogPlayServ, Warning, TEXT("PlayServ: external login rejected — credential token is empty"));
		Callback.ExecuteIfBound(false, TEXT(""), FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("PlayServ: external credential token is empty")));
		return;
	}

	if (Credential.Type == EPlayServExternalAuthType::SteamWebApiTicket
		&& !IsWebApiTicketHex(Credential.Token))
	{
		UE_LOG(LogPlayServ, Warning, TEXT("PlayServ: external login rejected — Steam ticket is not a hex string (length=%d)"), Credential.Token.Len());
		Callback.ExecuteIfBound(false, TEXT(""), FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("PlayServ: Steam ticket must be the hex-encoded GetAuthTicketForWebApi ticket (use TryMakeSteamCredential for raw bytes)")));
		return;
	}

	TSharedPtr<FJsonObject> Body = BuildV2LoginBody(Credential);

	TWeakObjectPtr<UPlayServAuth> WeakThis(this);
	const uint32 ExpectedGeneration = SessionGeneration;

	Http->Request(EPlayServHttpVerb::Post, TEXT("/auth/players/login"), Body, FPlayServV2Callback::CreateLambda(
		[WeakThis, ExpectedGeneration, Callback](bool bSuccess, const FPlayServHttpResponse& Response, const FPlayServError& Error)
		{
			UPlayServAuth* Self = WeakThis.Get();
			if (!Self)
			{
				return;
			}

			if (Self->SessionGeneration != ExpectedGeneration)
			{
				return;
			}

			if (!bSuccess)
			{
				UE_LOG(LogPlayServ, Warning, TEXT("PlayServ: login failed: %s"), *Error.Message);
				Callback.ExecuteIfBound(false, TEXT(""), Error);
				return;
			}

			FPlayServAuthResponse AuthResponse = FPlayServAuthResponse::FromV2Bundle(Response.Json);
			if (AuthResponse.AccessToken.IsEmpty())
			{
				UE_LOG(LogPlayServ, Warning, TEXT("PlayServ: login failed: server returned no access token"));
				Callback.ExecuteIfBound(false, TEXT(""), FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("Login failed: server returned no access token")));
				return;
			}

			Self->StartSession(AuthResponse, EPlayServSessionType::Client);
			Self->StartRefreshTimer();

			if (UPlayServSubsystem* Subsystem = UPlayServSubsystem::Get())
			{
				Subsystem->GetData()->RunSchemaAdvisoryCheck();
			}

			UE_LOG(LogPlayServ, Display, TEXT("PlayServ: login success (PlayerId=%s)"), *AuthResponse.PlayerId);
			Callback.ExecuteIfBound(true, AuthResponse.PlayerId, FPlayServError::Success());
		}), {}, EPlayServPlayerBearer::Suppress);
}

TSharedPtr<FJsonObject> UPlayServAuth::BuildV2LoginBody(const FPlayServExternalCredential& Credential)
{
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	switch (Credential.Type)
	{
	case EPlayServExternalAuthType::EpicAccessToken:
		Body->SetStringField(TEXT("provider"), TEXT("epic"));
		break;
	case EPlayServExternalAuthType::EpicLauncherExchangeCode:
		Body->SetStringField(TEXT("provider"), TEXT("epic"));
		Body->SetStringField(TEXT("mode"), TEXT("launcher_exchange_code"));
		break;
	case EPlayServExternalAuthType::SteamWebApiTicket:
		Body->SetStringField(TEXT("provider"), TEXT("steam"));
		break;
	default:
		break;
	}
	Body->SetStringField(TEXT("provider_token"), Credential.Token);

	const FString DisplayName = SanitizeDisplayName(Credential.DisplayName);
	if (!DisplayName.IsEmpty())
	{
		Body->SetStringField(DisplayNameField, DisplayName);
	}
	return Body;
}

TSharedPtr<FJsonObject> UPlayServAuth::BuildAnonLoginBody(const FString& DisplayName)
{
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();

	const FString Sanitized = SanitizeDisplayName(DisplayName);
	if (!Sanitized.IsEmpty())
	{
		Body->SetStringField(DisplayNameField, Sanitized);
	}
	return Body;
}

FString UPlayServAuth::SanitizeDisplayName(const FString& DisplayName)
{
	FString Sanitized = DisplayName.TrimStartAndEnd();
	if (Sanitized.Len() <= MaxDisplayNameLength)
	{
		return Sanitized;
	}

	int32 CutAt = MaxDisplayNameLength;

	if (StringConv::IsHighSurrogate(static_cast<uint32>(Sanitized[CutAt - 1])))
	{
		--CutAt;
	}
	Sanitized.LeftInline(CutAt);

	Sanitized.TrimEndInline();

	UE_LOG(LogPlayServ, Warning,
		TEXT("PlayServ: display name truncated from %d to %d characters (the SDK caps it at %d — the platform does not)"),
		DisplayName.Len(), Sanitized.Len(), MaxDisplayNameLength);

	return Sanitized;
}

namespace
{
	bool ParseSwitchValue(const TCHAR* CommandLine, const TCHAR* Switch, FString& OutValue)
	{
		const TCHAR* Found = FCString::Strifind(CommandLine, Switch, true);
		if (Found == nullptr)
		{
			return false;
		}

		const TCHAR* ValueStart = Found + FCString::Strlen(Switch);
		if (*ValueStart == TEXT('\0') || FChar::IsWhitespace(*ValueStart))
		{
			return false;
		}

		FString Parsed;
		if (!FParse::Value(CommandLine, Switch, Parsed))
		{
			return false;
		}

		Parsed = Parsed.TrimQuotes();
		if (Parsed.IsEmpty())
		{
			return false;
		}

		OutValue = MoveTemp(Parsed);
		return true;
	}
}

bool UPlayServAuth::ParseLauncherExchangeCode(const TCHAR* CommandLine, FString& OutExchangeCode)
{
	if (CommandLine == nullptr)
	{
		return false;
	}

	FString AuthType;
	if (!ParseSwitchValue(CommandLine, TEXT("-AUTH_TYPE="), AuthType))
	{
		return false;
	}

	if (!AuthType.Equals(TEXT("exchangecode"), ESearchCase::IgnoreCase))
	{
		return false;
	}

	FString ExchangeCode;
	if (!ParseSwitchValue(CommandLine, TEXT("-AUTH_PASSWORD="), ExchangeCode))
	{
		return false;
	}

	OutExchangeCode = MoveTemp(ExchangeCode);
	return true;
}

bool UPlayServAuth::TryGetLauncherCredential(FPlayServExternalCredential& OutCredential)
{
	FString ExchangeCode;
	if (!ParseLauncherExchangeCode(FCommandLine::Get(), ExchangeCode))
	{
		return false;
	}

	OutCredential.Type = EPlayServExternalAuthType::EpicLauncherExchangeCode;
	OutCredential.Token = MoveTemp(ExchangeCode);

	UE_LOG(LogPlayServ, Display, TEXT("PlayServ: Epic Games Launcher entry detected (exchange code present, value not logged)"));
	return true;
}

namespace
{
	constexpr int32 SteamWebApiTicketMaxBytes = 2560;
}

bool UPlayServAuth::IsWebApiTicketHex(const FString& Ticket)
{
	if (Ticket.IsEmpty() || Ticket.Len() % 2 != 0 || Ticket.Len() > SteamWebApiTicketMaxBytes * 2)
	{
		return false;
	}

	for (const TCHAR Character : Ticket)
	{
		if (!FChar::IsHexDigit(Character))
		{
			return false;
		}
	}

	return true;
}

bool UPlayServAuth::TryMakeSteamCredential(TConstArrayView<uint8> TicketBytes, FPlayServExternalCredential& OutCredential)
{
	if (TicketBytes.IsEmpty() || TicketBytes.Num() > SteamWebApiTicketMaxBytes)
	{
		UE_LOG(LogPlayServ, Warning, TEXT("PlayServ: Steam ticket rejected before encoding — %d bytes is outside 1..%d"),
			TicketBytes.Num(), SteamWebApiTicketMaxBytes);
		return false;
	}

	OutCredential.Type = EPlayServExternalAuthType::SteamWebApiTicket;
	OutCredential.Token = BytesToHex(TicketBytes.GetData(), TicketBytes.Num());

	UE_LOG(LogPlayServ, Display, TEXT("PlayServ: Steam web-API ticket encoded (%d bytes, value not logged)"), TicketBytes.Num());
	return true;
}

void UPlayServAuth::LoginWithRefreshToken(const FString& InRefreshToken, FPlayServAuthCallback Callback)
{
	UE_LOG(LogPlayServ, Display, TEXT("PlayServ: refresh-token login attempt"));
	ExchangeRefreshToken(InRefreshToken, ERefreshExchange::Login, MoveTemp(Callback));
}

bool UPlayServAuth::IsTerminalRefreshFailure(const FPlayServError& Error)
{
	switch (Error.Code)
	{
	case EPlayServErrorCode::Timeout:
	case EPlayServErrorCode::NetworkUnreachable:
		return false;
	default:
		break;
	}
	// The platform's front end answered for an app that was not there to take the request (a restart, a deploy) or asked
	// the client to slow down: the refresh was not processed, and the token is still the live one. Taken as terminal, one
	// such answer ended the session, so the game lost its crossings and the next launch made a new guest (Cube World, bug
	// hunt B16; the dev platform answered 5xx through its restarts on 2026-10-02). A 500 comes from the app itself.
	switch (Error.HttpStatus)
	{
	case 429:
	case 502:
	case 503:
	case 504:
		return false;
	default:
		return true;
	}
}

void UPlayServAuth::ExchangeRefreshToken(const FString& TokenToPresent, ERefreshExchange Mode, FPlayServAuthCallback Callback)
{
	const bool bIsLogin = Mode == ERefreshExchange::Login;

	if (!Http.IsValid())
	{
		if (bIsLogin)
		{
			Callback.ExecuteIfBound(false, TEXT(""), FPlayServError::Make(EPlayServErrorCode::NetworkUnreachable, TEXT("PlayServ HTTP not initialized")));
		}
		return;
	}

	if (TokenToPresent.IsEmpty())
	{
		if (bIsLogin)
		{
			UE_LOG(LogPlayServ, Warning, TEXT("PlayServ: refresh-token login rejected — token is empty"));
			Callback.ExecuteIfBound(false, TEXT(""), FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("PlayServ: refresh token is empty")));
		}
		return;
	}

	if (bRefreshInFlight)
	{
		if (bIsLogin)
		{
			UE_LOG(LogPlayServ, Warning, TEXT("PlayServ: refresh-token login rejected — a refresh-token exchange is already in flight"));
			Callback.ExecuteIfBound(false, TEXT(""), FPlayServError::Make(EPlayServErrorCode::NetworkUnreachable,
				TEXT("PlayServ: a refresh-token exchange is already in flight — presenting the same token twice would revoke the session family. Nothing was sent; the token is still valid. Wait for the first call to complete.")));
		}
		else
		{
			UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ: token refresh already in flight — skipping"));
		}
		return;
	}
	bRefreshInFlight = true;

	if (!bIsLogin)
	{
		UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ: token refresh (PlayerId=%s)"), *PlayerId);
	}

	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("refresh_token"), TokenToPresent);

	TWeakObjectPtr<UPlayServAuth> WeakThis(this);
	const uint32 ExpectedGeneration = SessionGeneration;

	const EPlayServPlayerBearer PlayerBearer = bIsLogin ? EPlayServPlayerBearer::Suppress : EPlayServPlayerBearer::Attach;

	Http->Request(EPlayServHttpVerb::Post, TEXT("/auth/players/refresh"), Body, FPlayServV2Callback::CreateLambda(
		[WeakThis, ExpectedGeneration, Callback, bIsLogin](bool bSuccess, const FPlayServHttpResponse& Response, const FPlayServError& Error)
		{
			UPlayServAuth* Self = WeakThis.Get();
			if (!Self)
			{
				return;
			}

			Self->bRefreshInFlight = false;

			if (Self->SessionGeneration != ExpectedGeneration)
			{
				return;
			}

			if (!bSuccess)
			{
				if (bIsLogin)
				{
					UE_LOG(LogPlayServ, Warning, TEXT("PlayServ: refresh-token login failed: %s"), *Error.Message);
					Callback.ExecuteIfBound(false, TEXT(""), Error);
					return;
				}

				if (!UPlayServAuth::IsTerminalRefreshFailure(Error))
				{
					UE_LOG(LogPlayServ, Warning,
						TEXT("PlayServ: token refresh not taken by the platform (%s) — session kept, will retry"), *Error.Message);
					Self->StartRefreshRetryTimer();
					return;
				}

				UE_LOG(LogPlayServ, Warning, TEXT("PlayServ: token refresh refused by the platform: %s — broadcasting OnSessionLost"), *Error.Message);
				Self->StopRefreshTimer();
				Self->ClearSession();
				Self->OnSessionLost.Broadcast();
				return;
			}

			FPlayServAuthResponse AuthResponse = FPlayServAuthResponse::FromV2Refresh(Response.Json);
			if (AuthResponse.AccessToken.IsEmpty() || AuthResponse.RefreshToken.IsEmpty())
			{
				if (bIsLogin)
				{
					UE_LOG(LogPlayServ, Warning, TEXT("PlayServ: refresh-token login failed: incomplete session bundle"));
					Callback.ExecuteIfBound(false, TEXT(""), FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("Login failed: server returned an incomplete session")));
					return;
				}

				UE_LOG(LogPlayServ, Warning, TEXT("PlayServ: token refresh failed: incomplete response — broadcasting OnSessionLost"));
				Self->StopRefreshTimer();
				Self->ClearSession();
				Self->OnSessionLost.Broadcast();
				return;
			}

			if (bIsLogin)
			{
				if (!UPlayServAuth::TryParsePlayerIdFromAccessToken(AuthResponse.AccessToken, AuthResponse.PlayerId))
				{
					UE_LOG(LogPlayServ, Warning, TEXT("PlayServ: refresh-token login failed: the issued access token carries no player id"));
					Callback.ExecuteIfBound(false, TEXT(""), FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("Login failed: the issued access token carries no player id")));
					return;
				}

				Self->StartSession(AuthResponse, EPlayServSessionType::Client);
			}
			else
			{
				Self->RefreshTokens(AuthResponse);
			}

			Self->RefreshRetryCount = 0;
			Self->StartRefreshTimer();

			if (bIsLogin)
			{
				if (UPlayServSubsystem* Subsystem = UPlayServSubsystem::Get())
				{
					Subsystem->GetData()->RunSchemaAdvisoryCheck();
				}

				UE_LOG(LogPlayServ, Display, TEXT("PlayServ: refresh-token login success (PlayerId=%s)"), *AuthResponse.PlayerId);
				Callback.ExecuteIfBound(true, AuthResponse.PlayerId, FPlayServError::Success());
			}
			else
			{
				UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ: token refresh success (PlayerId=%s)"), *Self->PlayerId);
			}
		}), {}, PlayerBearer);
}

void UPlayServAuth::SetRefreshTokenChangedHandler(FPlayServRefreshTokenChanged Handler)
{
	bRefreshTokenHandlerEverInstalled = Handler.IsBound();
	RefreshTokenChangedHandler = MoveTemp(Handler);
}

void UPlayServAuth::NotifyRefreshTokenChanged() const
{
	if (RefreshToken.IsEmpty())
	{
		return;
	}

	if (!RefreshTokenChangedHandler.IsBound())
	{
		if (bRefreshTokenHandlerEverInstalled)
		{
			UE_LOG(LogPlayServ, Warning,
				TEXT("PlayServ: the refresh token rotated but the host's handler is no longer bound — any copy it persisted earlier is now spent"));
		}
		return;
	}

	const FString TokenForHandler = RefreshToken;
	RefreshTokenChangedHandler.Execute(TokenForHandler);
}

bool UPlayServAuth::TryParsePlayerIdFromAccessToken(const FString& InAccessToken, FString& OutPlayerId)
{
	return PlayServJwt::TryReadStringClaim(InAccessToken, TEXT("sub"), OutPlayerId);
}

void UPlayServAuth::Logout(FPlayServSimpleCallback Callback, EPlayServLogoutMode Mode)
{
	UE_LOG(LogPlayServ, Display, TEXT("PlayServ: logout (PlayerId=%s, mode=%s)"), *PlayerId, Mode == EPlayServLogoutMode::Local ? TEXT("local") : TEXT("revoke"));

	OnLogoutEndsRealtime.ExecuteIfBound();

	if (Mode == EPlayServLogoutMode::Revoke && SessionType == EPlayServSessionType::Client && !RefreshToken.IsEmpty() && Http.IsValid())
	{
		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("refresh_token"), RefreshToken);

		StopRefreshTimer();
		ClearSession();

		Http->Request(EPlayServHttpVerb::Post, TEXT("/auth/players/sign-out"), Body, FPlayServV2Callback::CreateLambda(
			[Callback](bool bSuccess, const FPlayServHttpResponse&, const FPlayServError& Error)
			{
				if (!bSuccess)
				{
					UE_LOG(LogPlayServ, Warning, TEXT("PlayServ: server-side sign-out failed (%s) — local session already cleared"), *Error.Message);
				}
				Callback.ExecuteIfBound(true, FPlayServError::Success());
			}));
		return;
	}

	StopRefreshTimer();
	ClearSession();
	Callback.ExecuteIfBound(true, FPlayServError::Success());
}

void UPlayServAuth::LoginServer(const FString& ApiKey, FPlayServSimpleCallback Callback)
{
	if (ApiKey.IsEmpty())
	{
		Callback.ExecuteIfBound(false, FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("Server credential is empty")));
		return;
	}

	const bool bServerKey = ApiKey.StartsWith(TEXT("sk_"));
	if (!bServerKey && !IsJwtShaped(ApiKey))
	{
		Callback.ExecuteIfBound(false, FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("LoginServer requires an sk_* server key or a deployment token (client pk_* keys belong in UPlayServSettings::ClientKey)")));
		return;
	}
	UE_LOG(LogPlayServ, Display, TEXT("PlayServ: server login (static %s credential)"), bServerKey ? TEXT("sk_") : TEXT("deployment-token"));

	StopRefreshTimer();
	ClearSession();

	FPlayServAuthResponse Session;
	Session.AccessToken = ApiKey;
	StartSession(Session, EPlayServSessionType::Server);

	Callback.ExecuteIfBound(true, FPlayServError::Success());
}

bool UPlayServAuth::IsJwtShaped(const FString& Credential)
{
	return PlayServJwt::IsShaped(Credential);
}

const FString& UPlayServAuth::GetPlayerId() const
{
	return PlayerId;
}

const FString& UPlayServAuth::GetAccessToken() const
{
	return AccessToken;
}

bool UPlayServAuth::IsLoggedIn() const
{
	return SessionType != EPlayServSessionType::None;
}

bool UPlayServAuth::IsServerSession() const
{
	return SessionType == EPlayServSessionType::Server;
}

EPlayServSessionType UPlayServAuth::GetSessionType() const
{
	return SessionType;
}

void UPlayServAuth::StartSession(const FPlayServAuthResponse& AuthResponse, EPlayServSessionType Type)
{
	StopRefreshTimer();
	++SessionGeneration;
	AccessToken = AuthResponse.AccessToken;
	RefreshToken = AuthResponse.RefreshToken;
	PlayerId = AuthResponse.PlayerId;
	AccessTokenTTL = AuthResponse.AccessTokenTTL;
	RefreshTokenLifetime = AuthResponse.RefreshTokenLifetime;
	SessionType = Type;

	NotifyRefreshTokenChanged();
}

void UPlayServAuth::RefreshTokens(const FPlayServAuthResponse& AuthResponse)
{
	AccessToken = AuthResponse.AccessToken;
	RefreshToken = AuthResponse.RefreshToken;
	AccessTokenTTL = AuthResponse.AccessTokenTTL;
	RefreshTokenLifetime = AuthResponse.RefreshTokenLifetime;

	NotifyRefreshTokenChanged();
}

void UPlayServAuth::ClearSession()
{
	++SessionGeneration;
	AccessToken.Empty();
	RefreshToken.Empty();
	PlayerId.Empty();
	AccessTokenTTL = 0;
	RefreshTokenLifetime = 0;
	SessionType = EPlayServSessionType::None;
	bRefreshInFlight = false;
	RefreshRetryCount = 0;
}

void UPlayServAuth::StartRefreshTimer()
{
	StopRefreshTimer();

	if (AccessTokenTTL <= 0)
	{
		return;
	}

	const float RefreshDelay = static_cast<float>(AccessTokenTTL) * 0.8f;

	RefreshTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UPlayServAuth::OnRefreshTick),
		RefreshDelay);
}

bool UPlayServAuth::OnRefreshTick(float)
{
	RefreshTickerHandle.Reset();

	RefreshSession();
	return false;
}

void UPlayServAuth::StopRefreshTimer()
{
	if (RefreshTickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(RefreshTickerHandle);
		RefreshTickerHandle.Reset();
	}
}

void UPlayServAuth::RefreshSession()
{
	ExchangeRefreshToken(RefreshToken, ERefreshExchange::Rotate, FPlayServAuthCallback());
}

void UPlayServAuth::StartRefreshRetryTimer()
{
	StopRefreshTimer();

	constexpr float FirstRetrySeconds = 15.0f;
	constexpr float MaxRetrySeconds = 300.0f;

	++RefreshRetryCount;
	const int32 Doublings = FMath::Clamp(RefreshRetryCount - 1, 0, 8);
	const float Delay = FMath::Min(FirstRetrySeconds * static_cast<float>(1 << Doublings), MaxRetrySeconds);

	UE_LOG(LogPlayServ, Warning, TEXT("PlayServ: retrying the token refresh in %.0fs (attempt %d)"), Delay, RefreshRetryCount);

	RefreshTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UPlayServAuth::OnRefreshTick),
		Delay);
}
