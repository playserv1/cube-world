#include "PlayServTestCommon.h"
#include "Core/PlayServSubsystem.h"
#include "Auth/PlayServAuth.h"
#include "Code/PlayServRpcConverter.h"
#include "PlayServ.h"
#include "PlayServRpcTestTypes.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if !UE_BUILD_SHIPPING

// PlayServ.Code.TypedRPC.* — isolated strict-converter coverage (offline, no backend).
// See Specs/SDK/CloudFunctions/rpc.md.
//
// HTTP empty-body finding (verified against Core/PlayServHttp.cpp SendRequest): a 2xx response
// with an EMPTY body deserializes to an INVALID FJsonObject, but the malformed-JSON guard only
// fires when the body is non-empty. So an empty 2xx body reaches the callback as
// (bSuccess=true, Response=invalid). Overload A's "Response absent -> success with default
// TResponse" path keys off Result.Response.IsValid() being false — it is NOT swallowed as a
// malformed-JSON error.
//
// FBuyUpgradeRequest below is purely a serialization fixture; the live end-to-end coverage is
// PlayServ.Code.TypedRPC.LiveCreateAndBuy.

// PlayServ.Code.TypedRPC.StrictReject — JsonObjectToStruct gates on FJsonValue::Type.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServCodeStrictRejectTest,
	"PlayServ.Code.TypedRPC.StrictReject",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServCodeStrictRejectTest::RunTest(const FString& Parameters)
{
	// Numeric field carried as a STRING -> whole conversion fails (no silent coercion).
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("Quantity"), TEXT("450"));
		FRpcStrictProbe Probe;
		TestFalse(TEXT("numeric field as string is rejected"),
			PlayServ::Code::JsonObjectToStruct(Json, FRpcStrictProbe::StaticStruct(), &Probe));
	}

	// Bool field carried as a NUMBER (1) -> rejected.
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetNumberField(TEXT("bEnabled"), 1);
		FRpcStrictProbe Probe;
		TestFalse(TEXT("bool field as number is rejected"),
			PlayServ::Code::JsonObjectToStruct(Json, FRpcStrictProbe::StaticStruct(), &Probe));
	}

	// Absent fields are NOT a failure and keep their C++ defaults.
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("Tag"), TEXT("hello"));   // Quantity + bEnabled absent
		FRpcStrictProbe Probe;
		TestTrue(TEXT("partial response (absent fields) converts"),
			PlayServ::Code::JsonObjectToStruct(Json, FRpcStrictProbe::StaticStruct(), &Probe));
		TestEqual(TEXT("absent int keeps default (7)"), Probe.Quantity, 7);
		TestTrue(TEXT("absent bool keeps default (true)"), Probe.bEnabled);
		TestEqual(TEXT("present string applied"), Probe.Tag, FString(TEXT("hello")));
	}

	return true;
}

// PlayServ.Code.TypedRPC.VerbatimCasing — StructToRpcParams writes verbatim PascalCase keys.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServCodeVerbatimCasingTest,
	"PlayServ.Code.TypedRPC.VerbatimCasing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServCodeVerbatimCasingTest::RunTest(const FString& Parameters)
{
	FCreatePlayerFnRequest Req;
	Req.DisplayName = TEXT("CasingProbe");

	FPlayServRPCParams Params;
	TestTrue(TEXT("request serialized to params"),
		PlayServ::Code::StructToRpcParams(FCreatePlayerFnRequest::StaticStruct(), &Req, Params));

	TSharedPtr<FJsonObject> Json = Params.ToJson();
	TestTrue(TEXT("params produced a JSON object"), Json.IsValid());
	if (!Json.IsValid())
	{
		return false;
	}

	// FJsonObject key lookups are case-INsensitive — assert on the serialized text (case-sensitive).
	FString Serialized;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
	FJsonSerializer::Serialize(Json.ToSharedRef(), Writer);

	TestTrue(TEXT("DisplayName key verbatim"), Serialized.Contains(TEXT("\"DisplayName\""), ESearchCase::CaseSensitive));
	TestFalse(TEXT("no camelCase displayName leakage"), Serialized.Contains(TEXT("\"displayName\""), ESearchCase::CaseSensitive));
	return true;
}

// PlayServ.Code.TypedRPC.CamelCaseReplyBinds — the READ side of the casing contract. A camelCase
// reply binds to a PascalCase USTRUCT: FJsonObject::TryGetField is a case-insensitive lookup, so
// the C# SDK's default web-serializer casing ({"currency": 70}) lands in `Currency` with no
// [JsonPropertyName] on the reply. Pins the CloudFunctions spec statement: [JsonPropertyName]
// attributes on replies are belt-and-braces, not a binding requirement.
// VerbatimCasing above pins the REQUEST text; until this test the read side had no pin at all,
// which is how the opposite claim survived in four files.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServCodeCamelCaseReplyBindsTest,
	"PlayServ.Code.TypedRPC.CamelCaseReplyBinds",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServCodeCamelCaseReplyBindsTest::RunTest(const FString& Parameters)
{
	const FString ReplyText = TEXT("{\"currency\":70,\"upgradeLevels\":[1,2,3],\"purchasedType\":2,\"newLevel\":3,\"cost\":30}");
	TSharedPtr<FJsonObject> Json;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ReplyText);
	TestTrue(TEXT("camelCase reply text parses"), FJsonSerializer::Deserialize(Reader, Json) && Json.IsValid());
	if (!Json.IsValid())
	{
		return false;
	}

	FBuyUpgradeResponse Reply;
	TestTrue(TEXT("camelCase reply converts into the PascalCase USTRUCT"),
		PlayServ::Code::JsonObjectToStruct(Json, FBuyUpgradeResponse::StaticStruct(), &Reply));
	TestEqual(TEXT("currency -> Currency"), Reply.Currency, 70);
	TestEqual(TEXT("upgradeLevels -> UpgradeLevels (count)"), Reply.UpgradeLevels.Num(), 3);
	TestEqual(TEXT("purchasedType -> PurchasedType"), Reply.PurchasedType, 2);
	TestEqual(TEXT("newLevel -> NewLevel"), Reply.NewLevel, 3);
	TestEqual(TEXT("cost -> Cost"), Reply.Cost, 30);
	return true;
}

// PlayServ.Code.TypedRPC.SnakeCaseReplySilentlyDefaults — the casing that does NOT bind. A key
// that differs by more than case (snake_case — the platform's own REST convention) is simply
// absent to the strict reader: the conversion still reports success and the field keeps its
// C++ default. Pinned because this silently-defaulted success is the dangerous case the
// spec statement points at.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServCodeSnakeCaseReplySilentlyDefaultsTest,
	"PlayServ.Code.TypedRPC.SnakeCaseReplySilentlyDefaults",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServCodeSnakeCaseReplySilentlyDefaultsTest::RunTest(const FString& Parameters)
{
	const FString ReplyText = TEXT("{\"new_level\":3,\"purchased_type\":2,\"Cost\":30}");
	TSharedPtr<FJsonObject> Json;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ReplyText);
	TestTrue(TEXT("snake_case reply text parses"), FJsonSerializer::Deserialize(Reader, Json) && Json.IsValid());
	if (!Json.IsValid())
	{
		return false;
	}

	FBuyUpgradeResponse Reply;
	TestTrue(TEXT("conversion still reports success — absent fields are not an error"),
		PlayServ::Code::JsonObjectToStruct(Json, FBuyUpgradeResponse::StaticStruct(), &Reply));
	TestEqual(TEXT("new_level does NOT bind: NewLevel keeps its default"), Reply.NewLevel, 0);
	TestEqual(TEXT("purchased_type does NOT bind: PurchasedType keeps its default"), Reply.PurchasedType, 0);
	TestEqual(TEXT("the exactly-named Cost still binds"), Reply.Cost, 30);
	return true;
}

// PlayServ.Code.TypedRPC.NestedAndArray — round-trip nested USTRUCT + TArray + enum-by-name.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServCodeNestedAndArrayTest,
	"PlayServ.Code.TypedRPC.NestedAndArray",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServCodeNestedAndArrayTest::RunTest(const FString& Parameters)
{
	FRpcTestComposite Src;
	Src.Name = TEXT("composite");
	Src.Count = 5;
	Src.Color = ERpcTestColor::Blue;
	Src.Inner.Label = TEXT("inner");
	Src.Inner.Weight = 42;
	Src.Numbers = { 1, 2, 3 };
	{
		FRpcTestInner A; A.Label = TEXT("a"); A.Weight = 10;
		FRpcTestInner B; B.Label = TEXT("b"); B.Weight = 20;
		Src.Items = { A, B };
	}

	FPlayServRPCParams Params;
	TestTrue(TEXT("composite serialized"),
		PlayServ::Code::StructToRpcParams(FRpcTestComposite::StaticStruct(), &Src, Params));
	TSharedPtr<FJsonObject> Json = Params.ToJson();
	TestTrue(TEXT("json valid"), Json.IsValid());
	if (!Json.IsValid())
	{
		return false;
	}

	// Enum serializes by name, not ordinal.
	FString Serialized;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
	FJsonSerializer::Serialize(Json.ToSharedRef(), Writer);
	TestTrue(TEXT("enum written by name (Blue)"), Serialized.Contains(TEXT("\"Blue\""), ESearchCase::CaseSensitive));

	FRpcTestComposite Back;
	TestTrue(TEXT("composite deserialized"),
		PlayServ::Code::JsonObjectToStruct(Json, FRpcTestComposite::StaticStruct(), &Back));

	TestEqual(TEXT("Name roundtrip"), Back.Name, FString(TEXT("composite")));
	TestEqual(TEXT("Count roundtrip"), Back.Count, 5);
	TestEqual(TEXT("Color enum-by-name roundtrip"), static_cast<int32>(Back.Color), static_cast<int32>(ERpcTestColor::Blue));
	TestEqual(TEXT("Inner.Label roundtrip"), Back.Inner.Label, FString(TEXT("inner")));
	TestEqual(TEXT("Inner.Weight roundtrip"), Back.Inner.Weight, 42);
	TestEqual(TEXT("Numbers count"), Back.Numbers.Num(), 3);
	if (Back.Numbers.Num() == 3)
	{
		TestEqual(TEXT("Numbers[2]"), Back.Numbers[2], 3);
	}
	TestEqual(TEXT("Items count"), Back.Items.Num(), 2);
	if (Back.Items.Num() == 2)
	{
		TestEqual(TEXT("Items[0].Label"), Back.Items[0].Label, FString(TEXT("a")));
		TestEqual(TEXT("Items[1].Weight"), Back.Items[1].Weight, 20);
	}
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Code.TypedRPC.LiveCreateAndBuy — LIVE end-to-end over the deployed reference
// functions: anon player -> create-player (typed A; find-or-create returns the profile
// rec_* id; second call proves idempotency) -> buy-upgrade (typed A; server-authoritative
// currency math: 100 starting - 30 base cost = 70, level 0 -> 1) -> owner-scoped cleanup.
// Decoupled from any game module (wire-mirroring USTRUCTs in PlayServRpcTestTypes.h).
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServCodeTypedRpcLiveCreateAndBuyTest,
	"PlayServ.Code.TypedRPC.LiveCreateAndBuy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServCodeTypedRpcLiveCreateAndBuyTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("code-live-fn"));

	TSharedPtr<FString> ProfileId = MakeShared<FString>();
	TSharedPtr<bool> bFirstCreated = MakeShared<bool>(false);

	TSharedPtr<bool> bCreateDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("CreatePlayerFn"), [bCreateDone, ProfileId, bFirstCreated]()
	{
		FCreatePlayerFnRequest Request;
		Request.DisplayName = TEXT("TypedRpcBlob");
		PlayServ::Code::Call<FCreatePlayerFnRequest, FCreatePlayerFnResponse>(TEXT("create-player"), Request,
			[bCreateDone, ProfileId, bFirstCreated](bool bOk, const FCreatePlayerFnResponse& Resp, const FPlayServError&)
			{
				if (bOk)
				{
					*ProfileId = Resp.Id;
					*bFirstCreated = Resp.Created;
				}
				*bCreateDone = true;
			});
	}, bCreateDone, 15.0f));

	AddCommand(new FPlayServAssertStep(this, [ProfileId, bFirstCreated](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("create-player returned a rec_* profile id"), ProfileId->StartsWith(TEXT("rec_")));
		T->TestTrue(TEXT("first call created the profile (fresh anon caller)"), *bFirstCreated);
	}));

	// Idempotency: the second call finds the same row and creates nothing.
	TSharedPtr<FString> SecondId = MakeShared<FString>();
	TSharedPtr<bool> bSecondCreated = MakeShared<bool>(true);
	TSharedPtr<bool> bSecondDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("CreatePlayerFnAgain"), [bSecondDone, SecondId, bSecondCreated]()
	{
		FCreatePlayerFnRequest Request;
		Request.DisplayName = TEXT("MustBeIgnored");
		PlayServ::Code::Call<FCreatePlayerFnRequest, FCreatePlayerFnResponse>(TEXT("create-player"), Request,
			[bSecondDone, SecondId, bSecondCreated](bool bOk, const FCreatePlayerFnResponse& Resp, const FPlayServError&)
			{
				if (bOk)
				{
					*SecondId = Resp.Id;
					*bSecondCreated = Resp.Created;
				}
				*bSecondDone = true;
			});
	}, bSecondDone, 15.0f));

	AddCommand(new FPlayServAssertStep(this, [ProfileId, SecondId, bSecondCreated](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("idempotent: same profile id"), *SecondId, *ProfileId);
		T->TestFalse(TEXT("idempotent: nothing created on the second call"), *bSecondCreated);
	}));

	TSharedPtr<FBuyUpgradeResponse> Bought = MakeShared<FBuyUpgradeResponse>();
	TSharedPtr<bool> bBuyOk = MakeShared<bool>(false);
	TSharedPtr<bool> bBuyDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("BuyUpgradeFn"), [bBuyDone, bBuyOk, Bought]()
	{
		FBuyUpgradeRequest Request;
		Request.UpgradeType = 0;
		PlayServ::Code::Call<FBuyUpgradeRequest, FBuyUpgradeResponse>(TEXT("buy-upgrade"), Request,
			[bBuyDone, bBuyOk, Bought](bool bOk, const FBuyUpgradeResponse& Resp, const FPlayServError&)
			{
				*bBuyOk = bOk;
				*Bought = Resp;
				*bBuyDone = true;
			});
	}, bBuyDone, 15.0f));

	AddCommand(new FPlayServAssertStep(this, [bBuyOk, Bought](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("buy-upgrade succeeded"), *bBuyOk);
		T->TestEqual(TEXT("server math: 100 starting - 30 base cost"), Bought->Currency, 70);
		T->TestEqual(TEXT("upgrade slot 0 is level 1"), Bought->UpgradeLevels.IsValidIndex(0) ? Bought->UpgradeLevels[0] : -1, 1);
		T->TestEqual(TEXT("NewLevel echoes 1"), Bought->NewLevel, 1);
		T->TestEqual(TEXT("Cost echoes 30"), Bought->Cost, 30);
	}));

	// Cleanup: owner-scoped delete of the profile row (name-addressed low-level tier — the
	// suite deliberately has no game-module dependency), then logout.
	TSharedPtr<bool> bCleanDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("Cleanup"), [bCleanDone, ProfileId]()
	{
		if (ProfileId->IsEmpty())
		{
			*bCleanDone = true;
			return;
		}
		UPlayServSubsystem::Get()->GetData()->Delete(TEXT("BlobPlayerProfile"), *ProfileId,
			FPlayServSimpleCallback::CreateLambda([bCleanDone](bool, const FPlayServError&) { *bCleanDone = true; }));
	}, bCleanDone, 10.0f));

	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Code.UnknownFunctionFails — LIVE /fn error path: calling a slug
// with no deployed function is 404 `not_found` problem+json, mapped to
// EPlayServErrorCode::NotFound with the problem code preserved. Pins the /fn/{slug} route and
// the error mapping without needing a deployed function.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServCodeUnknownFunctionFailsTest,
	"PlayServ.Code.UnknownFunctionFails",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServCodeUnknownFunctionFailsTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("code-unknown-fn"));

	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	TSharedPtr<bool> bSuccess = MakeShared<bool>(true);
	TSharedPtr<FPlayServError> Error = MakeShared<FPlayServError>();
	AddCommand(new FPlayServAsyncStep(this, TEXT("CallUnknownFn"), [bDone, bSuccess, Error]()
	{
		PlayServ::Code::Call(TEXT("nonexistent-test-fn"), FPlayServRPCParams(),
			FPlayServRPCCallback::CreateLambda(
				[bDone, bSuccess, Error](bool bOk, const FPlayServRPCResult&, const FPlayServError& InError)
				{
					*bSuccess = bOk;
					*Error = InError;
					*bDone = true;
				}));
	}, bDone, 10.0f));

	AddCommand(new FPlayServAssertStep(this, [bSuccess, Error](FAutomationTestBase* T)
	{
		T->TestFalse(TEXT("unknown function fails"), *bSuccess);
		T->TestEqual(TEXT("mapped to NotFound"), Error->Code, EPlayServErrorCode::NotFound);
		T->TestEqual(TEXT("problem code preserved"), Error->ProblemCode, FString(TEXT("not_found")));
	}));

	return true;
}

#endif // !UE_BUILD_SHIPPING
