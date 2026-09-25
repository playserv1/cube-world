#include "Core/PlayServJwt.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Base64.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

bool PlayServJwt::IsShaped(const FString& Token)
{
	TArray<FString> Segments;
	Token.ParseIntoArray(Segments, TEXT("."), false);
	if (Segments.Num() != 3)
	{
		return false;
	}
	for (const FString& Segment : Segments)
	{
		if (Segment.IsEmpty())
		{
			return false;
		}
		for (const TCHAR Character : Segment)
		{
			const bool bBase64Url = (Character >= TEXT('A') && Character <= TEXT('Z'))
				|| (Character >= TEXT('a') && Character <= TEXT('z'))
				|| (Character >= TEXT('0') && Character <= TEXT('9'))
				|| Character == TEXT('-') || Character == TEXT('_') || Character == TEXT('=');
			if (!bBase64Url)
			{
				return false;
			}
		}
	}
	return true;
}

bool PlayServJwt::TryReadStringClaim(const FString& Token, const TCHAR* Claim, FString& OutValue)
{
	if (!IsShaped(Token))
	{
		return false;
	}

	TArray<FString> Segments;
	Token.ParseIntoArray(Segments, TEXT("."), false);

	FString Payload;
	if (!FBase64::Decode(Segments[1], Payload, EBase64Mode::UrlSafe))
	{
		return false;
	}

	TSharedPtr<FJsonObject> Claims;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Payload);
	if (!FJsonSerializer::Deserialize(Reader, Claims) || !Claims.IsValid())
	{
		return false;
	}

	const TSharedPtr<FJsonValue> Value = Claims->TryGetField(Claim);
	if (!Value.IsValid() || Value->Type != EJson::String)
	{
		return false;
	}

	FString ClaimValue = Value->AsString();
	if (ClaimValue.IsEmpty())
	{
		return false;
	}

	OutValue = MoveTemp(ClaimValue);
	return true;
}
