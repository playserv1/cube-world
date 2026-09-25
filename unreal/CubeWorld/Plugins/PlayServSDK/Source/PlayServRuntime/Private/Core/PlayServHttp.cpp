#include "Core/PlayServHttp.h"
#include "Core/PlayServErrorMapping.h"
#include "Core/PlayServLog.h"
#include "Core/PlayServSettings.h"
#include "Auth/PlayServAuth.h"
#include "HAL/PlatformTime.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

FPlayServHttp::FPlayServHttp()
	: Auth(nullptr)
{
}

void FPlayServHttp::SetAuth(UPlayServAuth* InAuth)
{
	Auth = InAuth;
}

void FPlayServHttp::SetServerBearerOverride(const FString& Bearer)
{
	ServerBearerOverride = Bearer;
}

FString FPlayServHttp::BuildUrl(const FString& Base, const FString& Path)
{
	FString CleanBase = Base;
	CleanBase.RemoveFromEnd(TEXT("/"));
	FString CleanPath = Path;
	if (!CleanPath.StartsWith(TEXT("/")))
	{
		CleanPath = TEXT("/") + CleanPath;
	}
	return CleanBase + CleanPath;
}

namespace
{
	const TCHAR* VerbString(EPlayServHttpVerb Verb)
	{
		switch (Verb)
		{
		case EPlayServHttpVerb::Get:    return TEXT("GET");
		case EPlayServHttpVerb::Post:   return TEXT("POST");
		case EPlayServHttpVerb::Patch:  return TEXT("PATCH");
		case EPlayServHttpVerb::Delete: return TEXT("DELETE");
		}
		return TEXT("POST");
	}

	void ExtractProblem(const TSharedPtr<FJsonObject>& Json, FString& OutCode, FString& OutDetail)
	{
		if (!Json.IsValid())
		{
			return;
		}
		Json->TryGetStringField(TEXT("code"), OutCode);
		if (!Json->TryGetStringField(TEXT("detail"), OutDetail))
		{
			Json->TryGetStringField(TEXT("title"), OutDetail);
		}
		const TSharedPtr<FJsonObject>* Errors;
		if (Json->TryGetObjectField(TEXT("errors"), Errors))
		{
			for (const auto& Pair : (*Errors)->Values)
			{
				const TArray<TSharedPtr<FJsonValue>>* Messages;
				if (Pair.Value.IsValid() && Pair.Value->TryGetArray(Messages))
				{
					for (const TSharedPtr<FJsonValue>& Msg : *Messages)
					{
						FString MsgStr;
						if (Msg->TryGetString(MsgStr))
						{
							OutDetail += FString::Printf(TEXT(" [%s: %s]"), *Pair.Key, *MsgStr);
						}
					}
				}
			}
		}
	}
}

#if !UE_BUILD_SHIPPING
namespace
{
	FPlayServHttp::FLastRequestForTest GLastRequestForTest;
}

const FPlayServHttp::FLastRequestForTest& FPlayServHttp::GetLastRequestForTest()
{
	return GLastRequestForTest;
}
#endif

void FPlayServHttp::Request(EPlayServHttpVerb Verb, const FString& Path, const TSharedPtr<FJsonObject>& Body, FPlayServV2Callback Callback, const TArray<FPlayServHeader>& ExtraHeaders, EPlayServPlayerBearer PlayerBearer, float DeadlineSeconds)
{
	const UPlayServSettings* Settings = UPlayServSettings::Get();
	if (!Settings)
	{
		Callback.ExecuteIfBound(false, FPlayServHttpResponse(), FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("PlayServ settings unavailable")));
		return;
	}

	const FString BaseURL = UPlayServSettings::GetBaseURL();
	const FString ClientKey = UPlayServSettings::GetClientKey();
	if (BaseURL.IsEmpty())
	{
		UE_LOG(LogPlayServ, Warning, TEXT("PlayServ HTTP: %s %s — rejected, BaseURL is not configured"), VerbString(Verb), *Path);
		Callback.ExecuteIfBound(false, FPlayServHttpResponse(), FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("PlayServ BaseURL is not configured")));
		return;
	}

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(BuildUrl(BaseURL, Path));
	Request->SetVerb(VerbString(Verb));

	UPlayServAuth* AuthPtr = Auth.Get();
	const bool bServerPlane = AuthPtr != nullptr && AuthPtr->IsServerSession() && PlayerBearer == EPlayServPlayerBearer::Attach;
	if (bServerPlane)
	{
		const FString& ServerBearer = ServerBearerOverride.IsEmpty() ? AuthPtr->GetAccessToken() : ServerBearerOverride;
		Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ServerBearer));
	}
	else
	{
		if (ClientKey.IsEmpty())
		{
			UE_LOG(LogPlayServ, Warning, TEXT("PlayServ HTTP: %s %s — rejected, ClientKey (pk_) is not configured"), VerbString(Verb), *Path);
			Callback.ExecuteIfBound(false, FPlayServHttpResponse(), FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("PlayServ ClientKey is not configured")));
			return;
		}
		Request->SetHeader(TEXT("X-PlayServ-Client"), ClientKey);
		if (AuthPtr != nullptr && PlayerBearer == EPlayServPlayerBearer::Attach)
		{
			const FString AccessToken = AuthPtr->GetAccessToken();
			if (!AccessToken.IsEmpty())
			{
				Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *AccessToken));
			}
		}
	}

	if (Body.IsValid())
	{
		FString BodyString;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyString);
		FJsonSerializer::Serialize(Body.ToSharedRef(), Writer);
		Request->SetContentAsString(BodyString);
		Request->SetHeader(TEXT("Content-Type"),
			Verb == EPlayServHttpVerb::Patch ? TEXT("application/merge-patch+json") : TEXT("application/json"));
	}
	else if (Verb != EPlayServHttpVerb::Get)
	{
		Request->SetHeader(TEXT("Content-Length"), TEXT("0"));
	}

	for (const FPlayServHeader& Header : ExtraHeaders)
	{
		Request->SetHeader(Header.Key, Header.Value);
	}

	if (DeadlineSeconds > 0.0f)
	{
		Request->SetTimeout(DeadlineSeconds);
		Request->SetActivityTimeout(DeadlineSeconds);
	}
	else if (Settings->RequestTimeoutSeconds > 0.0f)
	{
		Request->SetTimeout(Settings->RequestTimeoutSeconds);
	}

	const uint64 RequestId = ++RequestCounter;
	const double StartTime = FPlatformTime::Seconds();
	const FString VerbStr = VerbString(Verb);

	FString PathOnly = Path;
	int32 QueryIdx;
	if (PathOnly.FindChar(TEXT('?'), QueryIdx))
	{
		PathOnly.LeftInline(QueryIdx);
	}

	UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ HTTP: %s %s [#%llu]"), *VerbStr, *PathOnly, RequestId);

	Request->OnProcessRequestComplete().BindLambda(
		[Callback = MoveTemp(Callback), PathOnly, VerbStr, RequestId, StartTime](FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bConnectedSuccessfully)
		{
			const double Elapsed = FPlatformTime::Seconds() - StartTime;

			if (!bConnectedSuccessfully || !HttpResponse.IsValid())
			{
				const EHttpFailureReason Reason = HttpRequest.IsValid() ? HttpRequest->GetFailureReason() : EHttpFailureReason::None;
				UE_LOG(LogPlayServ, Warning, TEXT("PlayServ HTTP: %s %s [#%llu] — transport failure (%s) (%.2fs)"), *VerbStr, *PathOnly, RequestId, LexToString(Reason), Elapsed);
				Callback.ExecuteIfBound(false, FPlayServHttpResponse(), PlayServErrorMapping::FromTransportFailure(Reason, Elapsed));
				return;
			}

			FPlayServHttpResponse Response;
			Response.Status = HttpResponse->GetResponseCode();
			Response.ETag = HttpResponse->GetHeader(TEXT("ETag"));
			Response.Date = HttpResponse->GetHeader(TEXT("Date"));
			const FString RetryAfter = HttpResponse->GetHeader(TEXT("Retry-After"));
			if (!RetryAfter.IsEmpty() && RetryAfter.IsNumeric())
			{
				Response.RetryAfterSeconds = FCString::Atoi(*RetryAfter);
			}

			const FString ResponseBody = HttpResponse->GetContentAsString();
			if (!ResponseBody.IsEmpty())
			{
				TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseBody);
				if (!FJsonSerializer::Deserialize(Reader, Response.Json) || !Response.Json.IsValid())
				{
					Response.RawBody = ResponseBody;
				}
			}

			if (Response.Status >= 200 && Response.Status < 300)
			{
				UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ HTTP: %s %s [#%llu] — %d (%.2fs)"), *VerbStr, *PathOnly, RequestId, Response.Status, Elapsed);
				Callback.ExecuteIfBound(true, Response, FPlayServError::Success());
				return;
			}

			FString ProblemCode;
			FString Detail;
			ExtractProblem(Response.Json, ProblemCode, Detail);
			if (Detail.IsEmpty() && !Response.RawBody.IsEmpty())
			{
				Detail = Response.RawBody.Left(160);
			}
			Response.ProblemCode = ProblemCode;

			const FPlayServError Error = PlayServErrorMapping::FromV2Response(Response.Status, ProblemCode, Detail);
			UE_LOG(LogPlayServ, Warning, TEXT("PlayServ HTTP: %s %s [#%llu] — %s (%.2fs)"), *VerbStr, *PathOnly, RequestId, *Error.Message, Elapsed);
			Callback.ExecuteIfBound(false, Response, Error);
		});

#if !UE_BUILD_SHIPPING
	GLastRequestForTest.Verb = VerbStr;
	GLastRequestForTest.Path = PathOnly;
	GLastRequestForTest.bHasAuthorization = !Request->GetHeader(TEXT("Authorization")).IsEmpty();
	GLastRequestForTest.bHasClientKey = !Request->GetHeader(TEXT("X-PlayServ-Client")).IsEmpty();
	GLastRequestForTest.bHasContentLength = !Request->GetHeader(TEXT("Content-Length")).IsEmpty();
	GLastRequestForTest.TimeoutSeconds = Request->GetTimeout().Get(0.0f);
	GLastRequestForTest.ActivityTimeoutSeconds = DeadlineSeconds > 0.0f ? DeadlineSeconds : 0.0f;
#endif

	if (!Request->ProcessRequest())
	{
		UE_LOG(LogPlayServ, Warning, TEXT("PlayServ HTTP: %s %s [#%llu] — ProcessRequest() returned false, request not dispatched"), *VerbStr, *PathOnly, RequestId);
		Callback.ExecuteIfBound(false, FPlayServHttpResponse(), FPlayServError::Make(EPlayServErrorCode::NetworkUnreachable, TEXT("Failed to send HTTP request (not dispatched)")));
	}
}
