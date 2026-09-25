#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/DateTime.h"

struct FPlayServAuthResponse
{
	FString AccessToken;
	FString RefreshToken;
	FString PlayerId;
	int32 AccessTokenTTL = 0;
	int32 RefreshTokenLifetime = 0;

	static FPlayServAuthResponse FromV2Bundle(const TSharedPtr<FJsonObject>& Json)
	{
		FPlayServAuthResponse AuthResponse;
		if (!Json.IsValid())
		{
			return AuthResponse;
		}
		Json->TryGetStringField(TEXT("access_token"), AuthResponse.AccessToken);
		Json->TryGetStringField(TEXT("refresh_token"), AuthResponse.RefreshToken);
		Json->TryGetStringField(TEXT("player_id"), AuthResponse.PlayerId);
		double NumVal = 0.0;
		if (Json->TryGetNumberField(TEXT("expires_in"), NumVal))
		{
			AuthResponse.AccessTokenTTL = static_cast<int32>(NumVal);
		}
		if (Json->TryGetNumberField(TEXT("refresh_expires_in"), NumVal))
		{
			AuthResponse.RefreshTokenLifetime = static_cast<int32>(NumVal);
		}
		return AuthResponse;
	}

	static FPlayServAuthResponse FromV2Refresh(const TSharedPtr<FJsonObject>& Json)
	{
		FPlayServAuthResponse AuthResponse;
		if (!Json.IsValid())
		{
			return AuthResponse;
		}
		Json->TryGetStringField(TEXT("access_token"), AuthResponse.AccessToken);
		Json->TryGetStringField(TEXT("refresh_token"), AuthResponse.RefreshToken);
		double NumVal = 0.0;
		if (Json->TryGetNumberField(TEXT("refresh_expires_in"), NumVal))
		{
			AuthResponse.RefreshTokenLifetime = static_cast<int32>(NumVal);
		}
		FString ExpiresAt;
		FDateTime Parsed;
		if (Json->TryGetStringField(TEXT("expires_at"), ExpiresAt) && FDateTime::ParseIso8601(*ExpiresAt, Parsed))
		{
			AuthResponse.AccessTokenTTL = static_cast<int32>(FMath::Max<int64>(0, (Parsed - FDateTime::UtcNow()).GetTotalSeconds()));
		}
		return AuthResponse;
	}
};
