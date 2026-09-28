#pragma once

#include "CoreMinimal.h"
#include "Core/PlayServTypes.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/WeakObjectPtr.h"

class UPlayServAuth;

enum class EPlayServHttpVerb : uint8
{
	Get,
	Post,
	Patch,
	Delete
};

struct FPlayServHttpResponse
{
	int32 Status = 0;
	TSharedPtr<FJsonObject> Json;
	FString ProblemCode;
	FString ETag;
	FString Date;
	int32 RetryAfterSeconds = 0;
	FString RawBody;
};

DECLARE_DELEGATE_ThreeParams(FPlayServV2Callback, bool, const FPlayServHttpResponse&, const FPlayServError&);

using FPlayServHeader = TPair<FString, FString>;

enum class EPlayServPlayerBearer : uint8
{
	Attach,
	Suppress
};

class FPlayServHttp
{
public:
	FPlayServHttp();

	void SetAuth(UPlayServAuth* InAuth);

	void SetServerBearerOverride(const FString& Bearer);

	void Request(EPlayServHttpVerb Verb, const FString& Path, const TSharedPtr<FJsonObject>& Body, FPlayServV2Callback Callback, const TArray<FPlayServHeader>& ExtraHeaders = {}, EPlayServPlayerBearer PlayerBearer = EPlayServPlayerBearer::Attach, float DeadlineSeconds = 0.0f);

#if !UE_BUILD_SHIPPING
	struct FLastRequestForTest
	{
		FString Verb;
		FString Path;
		bool bHasAuthorization = false;
		bool bHasClientKey = false;
		bool bHasContentLength = false;
		float TimeoutSeconds = 0.0f;
		float ActivityTimeoutSeconds = 0.0f;
	};
	static PLAYSERVRUNTIME_API const FLastRequestForTest& GetLastRequestForTest();
#endif

private:
	static FString BuildUrl(const FString& Base, const FString& Path);

	TWeakObjectPtr<UPlayServAuth> Auth;

	FString ServerBearerOverride;

	uint64 RequestCounter = 0;
};
