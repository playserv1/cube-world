#pragma once

#include "PlayServTestCommon.h"
#include "Auth/PlayServAuth.h"
#include "Core/PlayServSettings.h"
#include "Core/PlayServSubsystem.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/ConfigCacheIni.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if !UE_BUILD_SHIPPING

// Raw wire access for tests that must pin the WIRE contract independent of the SDK's own
// plumbing (contract canaries, dead-token probes, fixture setup on the server plane).
// Requests are configured exactly like the SDK's transport. Server-plane calls read the
// server key from [PlayServ.DevSecrets] — test/tooling plane only; the runtime never reads
// that section.
namespace PlayServWireTest
{
	struct FWireResult
	{
		bool bCompleted = false;
		int32 Status = 0;
		TSharedPtr<FJsonObject> Json;
		FString Raw;
		FString ETag;

		FString Code() const
		{
			FString Value;
			if (Json.IsValid())
			{
				Json->TryGetStringField(TEXT("code"), Value);
			}
			return Value;
		}
	};

	using FWireResultPtr = TSharedPtr<FWireResult>;

	inline FString ServerKey()
	{
		FString Value;
		FString PlayServIni;
		FConfigCacheIni::LoadGlobalIniFile(PlayServIni, TEXT("PlayServ"));
		GConfig->GetString(TEXT("PlayServ.DevSecrets"), TEXT("ServerKey"), Value, PlayServIni);
		return Value;
	}

	// Fire one raw request against the V2 base URL. Credential modes:
	//   "client" -> X-PlayServ-Client: pk_
	//   "player" -> pk_ PLUS the live client session's player JWT (log in first)
	//   "server" -> Authorization: Bearer sk_
	//   "mixed"  -> BOTH (the contract says 401 invalid_credentials)
	//   "none"   -> no credentials
	inline void Fire(const FString& Verb, const FString& Path, const FString& CredMode,
		const TSharedPtr<FJsonObject>& Body, FWireResultPtr Result)
	{
		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(UPlayServSettings::GetBaseURL() + Path);
		Request->SetVerb(Verb);

		if (CredMode == TEXT("client") || CredMode == TEXT("player") || CredMode == TEXT("mixed"))
		{
			Request->SetHeader(TEXT("X-PlayServ-Client"), UPlayServSettings::GetClientKey());
		}
		if (CredMode == TEXT("player"))
		{
			const UPlayServSubsystem* PS = UPlayServSubsystem::Get();
			const UPlayServAuth* Auth = PS != nullptr ? PS->GetAuth() : nullptr;
			if (Auth != nullptr)
			{
				Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *Auth->GetAccessToken()));
			}
		}
		if (CredMode == TEXT("server") || CredMode == TEXT("mixed"))
		{
			Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *ServerKey()));
		}

		if (Body.IsValid())
		{
			FString BodyString;
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&BodyString);
			FJsonSerializer::Serialize(Body.ToSharedRef(), Writer);
			Request->SetContentAsString(BodyString);
			Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
		}

		Request->SetTimeout(15.0f);
		Request->OnProcessRequestComplete().BindLambda(
			[Result](FHttpRequestPtr, FHttpResponsePtr HttpResponse, bool bConnected)
			{
				if (bConnected && HttpResponse.IsValid())
				{
					Result->Status = HttpResponse->GetResponseCode();
					Result->Raw = HttpResponse->GetContentAsString();
					Result->ETag = HttpResponse->GetHeader(TEXT("ETag"));
					TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Result->Raw);
					FJsonSerializer::Deserialize(Reader, Result->Json);
				}
				Result->bCompleted = true;
			});
		Request->ProcessRequest();
	}

	// Fire-then-wait step pair: the async step dispatches and completes immediately; the poll
	// step waits on the wire result.
	inline void AddFireStep(FAutomationTestBase* Test, const FString& StepName, const FString& Verb,
		const FString& Path, const FString& CredMode, TSharedPtr<FJsonObject> Body, FWireResultPtr Result)
	{
		TSharedPtr<bool> bFired = MakeShared<bool>(false);
		Test->AddCommand(new FPlayServAsyncStep(Test, StepName, [Verb, Path, CredMode, Body, Result, bFired]()
		{
			Fire(Verb, Path, CredMode, Body, Result);
			*bFired = true;
		}, bFired));
		Test->AddCommand(new FPlayServPollStep(Test, StepName + TEXT("Wait"),
			[Result]() { return Result->bCompleted; }, 15.0f));
	}

	// Replace whatever session is live with a server session on the dev server key.
	inline void AddServerLoginStep(FAutomationTestBase* Test)
	{
		TSharedPtr<bool> bDone = MakeShared<bool>(false);
		Test->AddCommand(new FPlayServAsyncStep(Test, TEXT("LoginServer"), [Test, bDone]()
		{
			PlayServ::Auth::LoginServer(ServerKey(), FPlayServSimpleCallback::CreateLambda([Test, bDone](bool bOk, const FPlayServError& Error)
			{
				if (!bOk)
				{
					Test->AddError(FString::Printf(TEXT("LoginServer failed: %s"), *Error.Message));
				}
				*bDone = true;
			}));
		}, bDone, 8.0f));
	}

	// Resolve an entity's storage id (ent_*) by schema name from GET /data/tables.
	inline void AddResolveEntityStep(FAutomationTestBase* Test, const FString& EntityName, TSharedPtr<FString> OutEntityId)
	{
		FWireResultPtr Tables = MakeShared<FWireResult>();
		AddFireStep(Test, TEXT("ResolveEntity"), TEXT("GET"), TEXT("/data/tables"), TEXT("client"), nullptr, Tables);
		Test->AddCommand(new FPlayServAssertStep(Test, [Tables, EntityName, OutEntityId](FAutomationTestBase* T)
		{
			if (!Tables->Json.IsValid())
			{
				T->AddError(TEXT("GET /data/tables returned no JSON"));
				return;
			}
			const TArray<TSharedPtr<FJsonValue>>* Data;
			if (!Tables->Json->TryGetArrayField(TEXT("data"), Data))
			{
				T->AddError(TEXT("tables envelope missing 'data'"));
				return;
			}
			for (const TSharedPtr<FJsonValue>& Entry : *Data)
			{
				const TSharedPtr<FJsonObject>* Obj;
				if (Entry->TryGetObject(Obj) && (*Obj)->GetStringField(TEXT("name")) == EntityName)
				{
					*OutEntityId = (*Obj)->GetStringField(TEXT("entity_id"));
					return;
				}
			}
			T->AddError(FString::Printf(TEXT("%s not found in /data/tables — dev schema missing the fixture push?"), *EntityName));
		}));
	}
}

#endif // !UE_BUILD_SHIPPING
