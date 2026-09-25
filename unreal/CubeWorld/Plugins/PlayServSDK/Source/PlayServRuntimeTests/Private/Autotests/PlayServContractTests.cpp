#include "PlayServTestCommon.h"
#include "PlayServWireTestHelpers.h"
#include "PlayServRoomsTestHelpers.h"
#include "Core/PlayServHttp.h"
#include "Rooms/PlayServRoomsClientWire.h"
#include "Rooms/PlayServRoomsPaths.h"
#include "Rooms/PlayServUplinkClient.h"
#include "Rooms/PlayServUplinkTransport.h"
#include "Misc/Guid.h"

#if !UE_BUILD_SHIPPING

// ---------------------------------------------------------------------------
// PlayServ.Contract.* — wire-contract canaries
//
// These pin the platform's wire facts against the LIVE environment and stay in the suite
// forever as drift alarms: if the platform changes an envelope, an error code, or the
// credential rules, these fail before any feature test gets confusing. They deliberately
// assert the REAL wire, including the places where the platform's own contract docs are
// known to disagree with its code (page envelope field names, `not_found` vs the documented
// `record_not_found`).
//
// Transport: canaries that a game client could issue run through the SDK's V2 tier
// (FPlayServHttp is Private/, so via a thin raw-HTTP helper configured EXACTLY like it —
// these pin the WIRE, not the SDK plumbing). Server-plane canaries read the server key from
// [PlayServ.DevSecrets] (test/tooling plane only — never the runtime).
// ---------------------------------------------------------------------------

using namespace PlayServWireTest;

// TestItem's storage id, resolved once per canary that needs it.
static void AddResolveTestItemStep(FAutomationTestBase* Test, TSharedPtr<FString> OutEntityId)
{
	AddResolveEntityStep(Test, TEXT("TestItem"), OutEntityId);
}

// ---------------------------------------------------------------------------
// TablesEnvelope — GET /data/tables (pk_ only; deliberately ungated): envelope is
// {data:[...]}, entries carry entity_id/name/read/acl, and the DOCUMENTED-but-wrong `items`
// key is absent.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServContractTablesEnvelopeTest,
	"PlayServ.Contract.TablesEnvelope",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServContractTablesEnvelopeTest::RunTest(const FString& Parameters)
{
	FWireResultPtr Result = MakeShared<FWireResult>();
	TSharedPtr<bool> bFired = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("GetTables"), [Result, bFired]()
	{
		Fire(TEXT("GET"), TEXT("/data/tables"), TEXT("client"), nullptr, Result);
		*bFired = true;
	}, bFired));
	AddCommand(new FPlayServPollStep(this, TEXT("GetTablesWait"), [Result]() { return Result->bCompleted; }, 15.0f));
	AddCommand(new FPlayServAssertStep(this, [Result](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("200 OK"), Result->Status, 200);
		if (!Result->Json.IsValid())
		{
			T->AddError(TEXT("no JSON body"));
			return;
		}
		T->TestFalse(TEXT("documented-but-wrong 'items' key absent (drift alarm)"), Result->Json->HasField(TEXT("items")));
		const TArray<TSharedPtr<FJsonValue>>* Data;
		if (!T->TestTrue(TEXT("envelope has 'data' array"), Result->Json->TryGetArrayField(TEXT("data"), Data)))
		{
			return;
		}
		T->TestTrue(TEXT("at least one table"), Data->Num() > 0);
		const TSharedPtr<FJsonObject>* First;
		if ((*Data)[0]->TryGetObject(First))
		{
			T->TestTrue(TEXT("entry carries entity_id"), (*First)->HasField(TEXT("entity_id")));
			T->TestTrue(TEXT("entry carries name"), (*First)->HasField(TEXT("name")));
			T->TestTrue(TEXT("entry carries read policy"), (*First)->HasField(TEXT("read")));
			T->TestTrue(TEXT("entry carries acl"), (*First)->HasField(TEXT("acl")));
			T->TestFalse(TEXT("entry has no 'kind' (contract drift pinned)"), (*First)->HasField(TEXT("kind")));
		}
	}));
	return true;
}

// ---------------------------------------------------------------------------
// NotFoundCode — GET on a nonexistent record 404s with code `not_found` (the documented
// `record_not_found` does NOT exist on the wire; this canary pins the real spelling).
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServContractNotFoundCodeTest,
	"PlayServ.Contract.NotFoundCode",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServContractNotFoundCodeTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FString> EntityId = MakeShared<FString>();
	AddResolveTestItemStep(this, EntityId);

	FWireResultPtr Result = MakeShared<FWireResult>();
	TSharedPtr<bool> bFired = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("GetMissing"), [Result, EntityId, bFired]()
	{
		// Server plane so the read is not blocked by TestItem's closed client ACL — the
		// canary targets the not-found path, not the ACL gate.
		Fire(TEXT("GET"), FString::Printf(TEXT("/data/tables/%s/records/rec_00000000000000000000000000"), **EntityId), TEXT("server"), nullptr, Result);
		*bFired = true;
	}, bFired));
	AddCommand(new FPlayServPollStep(this, TEXT("GetMissingWait"), [Result]() { return Result->bCompleted; }, 15.0f));
	AddCommand(new FPlayServAssertStep(this, [Result](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("404"), Result->Status, 404);
		T->TestEqual(TEXT("code is not_found (not record_not_found)"), Result->Code(), FString(TEXT("not_found")));
	}));
	return true;
}

// ---------------------------------------------------------------------------
// QueryLimitCeiling — records:query with limit 500 is REFUSED with `400 limit_out_of_range`
// (platform `conventions.md` §6). This test previously asserted the opposite —
// that 500 was tolerated and silently clamped to 200 — which is the drift the platform
// closed: the ceiling is now declared once (OpenAPI `parameters.yaml#/Limit`, `maximum: 200`)
// and a value outside 1..200 is rejected rather than quietly rewritten. Read more rows by
// walking `cursor`, which is what the SDK's own read path does.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServContractQueryLimitCeilingTest,
	"PlayServ.Contract.QueryLimitCeiling",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServContractQueryLimitCeilingTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FString> EntityId = MakeShared<FString>();
	AddResolveTestItemStep(this, EntityId);

	FWireResultPtr Result = MakeShared<FWireResult>();
	TSharedPtr<bool> bFired = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("Query500"), [Result, EntityId, bFired]()
	{
		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetNumberField(TEXT("limit"), 500);
		Fire(TEXT("POST"), FString::Printf(TEXT("/data/tables/%s/records:query"), **EntityId), TEXT("server"), Body, Result);
		*bFired = true;
	}, bFired));
	AddCommand(new FPlayServPollStep(this, TEXT("Query500Wait"), [Result]() { return Result->bCompleted; }, 15.0f));
	AddCommand(new FPlayServAssertStep(this, [Result](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("400 (limit 500 is refused, not clamped)"), Result->Status, 400);
		T->TestEqual(TEXT("code is limit_out_of_range"), Result->Code(), FString(TEXT("limit_out_of_range")));
		if (!Result->Json.IsValid())
		{
			T->AddError(TEXT("no JSON body"));
			return;
		}
		T->TestTrue(TEXT("problem names the requested limit"), Result->Json->HasField(TEXT("limit")));
		T->TestTrue(TEXT("problem names the ceiling"), Result->Json->HasField(TEXT("max_limit")));
	}));
	return true;
}


// ---------------------------------------------------------------------------
// QueryPageEnvelope — records:query at the ceiling (limit 200) is accepted and returns the
// REAL page envelope {data, page{cursor_next,cursor_prev,has_more}, total_estimate}.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServContractQueryPageEnvelopeTest,
	"PlayServ.Contract.QueryPageEnvelope",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServContractQueryPageEnvelopeTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FString> EntityId = MakeShared<FString>();
	AddResolveTestItemStep(this, EntityId);

	FWireResultPtr Result = MakeShared<FWireResult>();
	TSharedPtr<bool> bFired = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("Query200"), [Result, EntityId, bFired]()
	{
		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetNumberField(TEXT("limit"), 200);
		Fire(TEXT("POST"), FString::Printf(TEXT("/data/tables/%s/records:query"), **EntityId), TEXT("server"), Body, Result);
		*bFired = true;
	}, bFired));
	AddCommand(new FPlayServPollStep(this, TEXT("Query200Wait"), [Result]() { return Result->bCompleted; }, 15.0f));
	AddCommand(new FPlayServAssertStep(this, [Result](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("200 OK (limit 200 is the ceiling, accepted)"), Result->Status, 200);
		if (!Result->Json.IsValid())
		{
			T->AddError(TEXT("no JSON body"));
			return;
		}
		const TArray<TSharedPtr<FJsonValue>>* Data;
		if (T->TestTrue(TEXT("envelope has 'data'"), Result->Json->TryGetArrayField(TEXT("data"), Data)))
		{
			T->TestTrue(TEXT("page never exceeds the ceiling"), Data->Num() <= 200);
		}
		const TSharedPtr<FJsonObject>* Page;
		if (T->TestTrue(TEXT("envelope has 'page'"), Result->Json->TryGetObjectField(TEXT("page"), Page)))
		{
			T->TestTrue(TEXT("page has has_more"), (*Page)->HasField(TEXT("has_more")));
			T->TestTrue(TEXT("page has cursor_next"), (*Page)->HasField(TEXT("cursor_next")));
			T->TestTrue(TEXT("page has cursor_prev"), (*Page)->HasField(TEXT("cursor_prev")));
		}
		T->TestTrue(TEXT("envelope has total_estimate"), Result->Json->HasField(TEXT("total_estimate")));
		T->TestFalse(TEXT("documented-but-wrong 'next_cursor' absent (drift alarm)"), Result->Json->HasField(TEXT("next_cursor")));
	}));
	return true;
}

// ---------------------------------------------------------------------------
// MixedCredentialsRejected — pk_ + sk_ on one request is 401 `invalid_credentials`
// (mutually-exclusive credential planes).
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServContractMixedCredentialsTest,
	"PlayServ.Contract.MixedCredentialsRejected",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServContractMixedCredentialsTest::RunTest(const FString& Parameters)
{
	FWireResultPtr Result = MakeShared<FWireResult>();
	TSharedPtr<bool> bFired = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("MixedCreds"), [Result, bFired]()
	{
		Fire(TEXT("GET"), TEXT("/data/tables"), TEXT("mixed"), nullptr, Result);
		*bFired = true;
	}, bFired));
	AddCommand(new FPlayServPollStep(this, TEXT("MixedCredsWait"), [Result]() { return Result->bCompleted; }, 15.0f));
	AddCommand(new FPlayServAssertStep(this, [Result](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("401"), Result->Status, 401);
		T->TestEqual(TEXT("code is invalid_credentials"), Result->Code(), FString(TEXT("invalid_credentials")));
	}));
	return true;
}

// ---------------------------------------------------------------------------
// AnonBundleShape — POST /auth/players/anon (pk_ only) mints a full token bundle with the
// six contract fields, including refresh_expires_in.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServContractAnonBundleShapeTest,
	"PlayServ.Contract.AnonBundleShape",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServContractAnonBundleShapeTest::RunTest(const FString& Parameters)
{
	FWireResultPtr Result = MakeShared<FWireResult>();
	TSharedPtr<bool> bFired = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("AnonMint"), [Result, bFired]()
	{
		Fire(TEXT("POST"), TEXT("/auth/players/anon"), TEXT("client"), MakeShared<FJsonObject>(), Result);
		*bFired = true;
	}, bFired));
	AddCommand(new FPlayServPollStep(this, TEXT("AnonMintWait"), [Result]() { return Result->bCompleted; }, 15.0f));
	AddCommand(new FPlayServAssertStep(this, [Result](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("200 OK"), Result->Status, 200);
		if (!Result->Json.IsValid())
		{
			T->AddError(TEXT("no JSON body"));
			return;
		}
		for (const TCHAR* Field : { TEXT("player_id"), TEXT("access_token"), TEXT("refresh_token"),
			TEXT("expires_in"), TEXT("issued_at"), TEXT("refresh_expires_in") })
		{
			T->TestTrue(FString::Printf(TEXT("bundle carries %s"), Field), Result->Json->HasField(Field));
		}
		FString PlayerId;
		Result->Json->TryGetStringField(TEXT("player_id"), PlayerId);
		T->TestTrue(TEXT("player_id is plr_*"), PlayerId.StartsWith(TEXT("plr_")));
	}));
	return true;
}

// ---------------------------------------------------------------------------
// UnknownFieldErrorsMap — a write carrying an undeclared field is 422 `unknown_field` with
// the per-field errors map (field -> string[]). Server plane; nothing is persisted.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServContractUnknownFieldErrorsMapTest,
	"PlayServ.Contract.UnknownFieldErrorsMap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServContractUnknownFieldErrorsMapTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FString> EntityId = MakeShared<FString>();
	AddResolveTestItemStep(this, EntityId);

	FWireResultPtr Result = MakeShared<FWireResult>();
	TSharedPtr<bool> bFired = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("BogusWrite"), [Result, EntityId, bFired]()
	{
		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetNumberField(TEXT("BogusFieldNobodyDeclared"), 1);
		Fire(TEXT("POST"), FString::Printf(TEXT("/data/tables/%s/records"), **EntityId), TEXT("server"), Body, Result);
		*bFired = true;
	}, bFired));
	AddCommand(new FPlayServPollStep(this, TEXT("BogusWriteWait"), [Result]() { return Result->bCompleted; }, 15.0f));
	AddCommand(new FPlayServAssertStep(this, [Result](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("422"), Result->Status, 422);
		T->TestEqual(TEXT("code is unknown_field"), Result->Code(), FString(TEXT("unknown_field")));
		if (!Result->Json.IsValid())
		{
			return;
		}
		const TSharedPtr<FJsonObject>* Errors;
		if (T->TestTrue(TEXT("problem carries errors map"), Result->Json->TryGetObjectField(TEXT("errors"), Errors)))
		{
			const TArray<TSharedPtr<FJsonValue>>* FieldErrors;
			T->TestTrue(TEXT("errors[field] is a string array"),
				(*Errors)->TryGetArrayField(TEXT("BogusFieldNobodyDeclared"), FieldErrors) && FieldErrors->Num() > 0);
		}
	}));
	return true;
}

// ---------------------------------------------------------------------------
// UnknownFieldInPartErrorsMap — an undeclared field INSIDE an inclusion-part
// row is also 422 `unknown_field`, and the errors map is keyed by the BARE part-field name
// ("BogusField"), NOT a nested path ("Slots.BogusField" / "Slots[0].BogusField"); the human
// detail names the PART ("InventorySlot"), not the entity. Pinned live 2026-08-15: a client
// mapping errors back to properties must expect the flat key. Nothing is persisted.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServContractUnknownFieldInPartErrorsMapTest,
	"PlayServ.Contract.UnknownFieldInPartErrorsMap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServContractUnknownFieldInPartErrorsMapTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FString> EntityId = MakeShared<FString>();
	AddResolveEntityStep(this, TEXT("TestInventory"), EntityId);

	FWireResultPtr Result = MakeShared<FWireResult>();
	TSharedPtr<bool> bFired = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("BogusPartWrite"), [Result, EntityId, bFired]()
	{
		TSharedPtr<FJsonObject> Slot = MakeShared<FJsonObject>();
		Slot->SetStringField(TEXT("Item"), TEXT("rec_00000000000000000000000000"));
		Slot->SetNumberField(TEXT("Count"), 1);
		Slot->SetStringField(TEXT("Notes"), TEXT("canary"));
		Slot->SetStringField(TEXT("BogusField"), TEXT("undeclared"));

		TArray<TSharedPtr<FJsonValue>> Slots;
		Slots.Add(MakeShared<FJsonValueObject>(Slot));

		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetNumberField(TEXT("MaxSlots"), 5);
		Body->SetArrayField(TEXT("Slots"), Slots);
		Fire(TEXT("POST"), FString::Printf(TEXT("/data/tables/%s/records"), **EntityId), TEXT("server"), Body, Result);
		*bFired = true;
	}, bFired));
	AddCommand(new FPlayServPollStep(this, TEXT("BogusPartWriteWait"), [Result]() { return Result->bCompleted; }, 15.0f));
	AddCommand(new FPlayServAssertStep(this, [Result](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("422"), Result->Status, 422);
		T->TestEqual(TEXT("code is unknown_field"), Result->Code(), FString(TEXT("unknown_field")));
		if (!Result->Json.IsValid())
		{
			return;
		}
		const TSharedPtr<FJsonObject>* Errors;
		if (T->TestTrue(TEXT("problem carries errors map"), Result->Json->TryGetObjectField(TEXT("errors"), Errors)))
		{
			const TArray<TSharedPtr<FJsonValue>>* FieldErrors;
			T->TestTrue(TEXT("errors keyed by the BARE part-field name"),
				(*Errors)->TryGetArrayField(TEXT("BogusField"), FieldErrors) && FieldErrors->Num() > 0);
			T->TestFalse(TEXT("no nested-path key (drift alarm)"), (*Errors)->HasField(TEXT("Slots.BogusField")));
		}
		FString Detail;
		if (Result->Json->TryGetStringField(TEXT("detail"), Detail))
		{
			T->TestTrue(TEXT("detail names the PART (InventorySlot), not the entity"), Detail.Contains(TEXT("InventorySlot")));
		}
	}));
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Contract.RoomsBrowseEnvelope — GET /rooms/{slug}:browse as a signed-in player.
//
// BR-15: one canary per field the SDK reads. The client half parses `data` and
// `page.cursor_next` / `page.has_more`; a room's own fields are asserted when the dev
// environment has a room registered, and skipped (not failed) when it is empty — this suite
// runs against a shared cluster where no server may be up.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServContractRoomsBrowseTest,
	"PlayServ.Contract.RoomsBrowseEnvelope",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServContractRoomsBrowseTest::RunTest(const FString& Parameters)
{
	AddLoginStep(this, TEXT("RoomsBrowse"));

	FWireResultPtr Result = MakeShared<FWireResult>();
	AddFireStep(this, TEXT("BrowseRooms"), TEXT("GET"),
		TEXT("/rooms/blob-arena:browse?placement_state=open"), TEXT("player"), nullptr, Result);

	AddCommand(new FPlayServAssertStep(this, [Result](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("200 OK"), Result->Status, 200);
		if (!Result->Json.IsValid())
		{
			T->AddError(TEXT("browse returned no JSON body"));
			return;
		}

		// The SDK's own parser must accept exactly what the platform sent.
		FPlayServBrowsePage Page;
		T->TestTrue(TEXT("the SDK parses the live envelope"), PlayServRoomsClientWire::ParseBrowsePage(Result->Json, Page));

		const TSharedPtr<FJsonObject>* PageObject = nullptr;
		if (!T->TestTrue(TEXT("the envelope carries a `page` object"), Result->Json->TryGetObjectField(TEXT("page"), PageObject)))
		{
			return;
		}
		T->TestTrue(TEXT("page.cursor_next is served (the SDK's NextCursor)"), (*PageObject)->HasField(TEXT("cursor_next")));
		T->TestTrue(TEXT("page.has_more is served"), (*PageObject)->HasField(TEXT("has_more")));

		const TArray<TSharedPtr<FJsonValue>>* Data = nullptr;
		if (!Result->Json->TryGetArrayField(TEXT("data"), Data) || Data->Num() == 0)
		{
			T->AddInfo(TEXT("no room is registered on dev right now — the per-room fields are not asserted this run"));
			return;
		}
		const TSharedPtr<FJsonObject>* Room = nullptr;
		if (!(*Data)[0]->TryGetObject(Room))
		{
			T->AddError(TEXT("a browse item is not an object"));
			return;
		}
		for (const TCHAR* Field : { TEXT("room_name"), TEXT("players"), TEXT("capacity"), TEXT("placement_state") })
		{
			T->TestTrue(FString::Printf(TEXT("RoomBrowseItem.%s is served"), Field), (*Room)->HasField(Field));
		}
		// The client projection deliberately hides the server plane's internals.
		T->TestFalse(TEXT("no instance_id in the client projection (drift alarm)"), (*Room)->HasField(TEXT("instance_id")));
	}));

	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Contract.RoomsJoinUnknownRoom — POST /rooms/{slug}/{roomName}:join for a room that
// does not exist answers `404 room_not_found` as RFC 7807 problem details. The client half
// branches on that code, so its spelling is load-bearing.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServContractRoomsJoinUnknownTest,
	"PlayServ.Contract.RoomsJoinUnknownRoom",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServContractRoomsJoinUnknownTest::RunTest(const FString& Parameters)
{
	AddLoginStep(this, TEXT("RoomsJoin"));

	FWireResultPtr Result = MakeShared<FWireResult>();
	AddFireStep(this, TEXT("JoinUnknownRoom"), TEXT("POST"),
		TEXT("/rooms/blob-arena/no-such-room:join"), TEXT("player"), MakeShared<FJsonObject>(), Result);

	AddCommand(new FPlayServAssertStep(this, [Result](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("404"), Result->Status, 404);
		T->TestEqual(TEXT("problem code is room_not_found"), Result->Code(), FString(TEXT("room_not_found")));
	}));

	// And an unknown room TYPE is a different code — the SDK reports them differently.
	FWireResultPtr UnknownType = MakeShared<FWireResult>();
	AddFireStep(this, TEXT("BrowseUnknownType"), TEXT("GET"),
		TEXT("/rooms/no-such-room-type:browse"), TEXT("player"), nullptr, UnknownType);
	AddCommand(new FPlayServAssertStep(this, [UnknownType](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("404"), UnknownType->Status, 404);
		T->TestEqual(TEXT("problem code is room_type_not_found"), UnknownType->Code(), FString(TEXT("room_type_not_found")));
	}));

	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Contract.RoomsHostRefusals  (PSV-2693) — POST /rooms/{slug}:host, side-effect free.
//
// Every request here is one the platform refuses BEFORE it would start anything: an unknown room
// type, a body that names the room (the platform mints names), and nested attributes. The first
// goes through the SDK's own RequestNewRoom, so the deadline it put on the real transport is asserted
// as well — `:host` holds its answer until a started server registers, and neither the SDK's 20 s
// request timeout nor the engine's 30 s idle timeout may cut that. A request the platform would
// ACCEPT starts a server on a pool machine, so the suite never sends one.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServContractRoomsHostTest,
	"PlayServ.Contract.RoomsHostRefusals",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServContractRoomsHostTest::RunTest(const FString& Parameters)
{
	AddLoginStep(this, TEXT("RoomsHost"));

	struct FCreateOutcome
	{
		bool bFired = false;
		bool bSuccess = true;
		FPlayServError Error;
		FPlayServHttp::FLastRequestForTest Sent;
	};
	TSharedPtr<FCreateOutcome> Outcome = MakeShared<FCreateOutcome>();
	TSharedPtr<bool> bIssued = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("RequestNewRoomOfUnknownType"), [Outcome, bIssued]()
	{
		TMap<FString, FString> Attributes;
		Attributes.Add(TEXT("map"), TEXT("BlobArenaMap"));
		FPlayServRoomsTestAccess::RequestNewRoomOfType(UPlayServSubsystem::Get()->GetRooms(), TEXT("no-such-room-type"), Attributes, FPlayServRequestNewRoomCallback::CreateLambda(
			[Outcome](bool bSuccess, const FPlayServRoomListing&, const FPlayServError& Error)
			{
				Outcome->bFired = true;
				Outcome->bSuccess = bSuccess;
				Outcome->Error = Error;
			}));
		// Read at once: the next request anywhere in the process overwrites it.
		Outcome->Sent = FPlayServHttp::GetLastRequestForTest();
		*bIssued = true;
	}, bIssued));
	AddCommand(new FPlayServPollStep(this, TEXT("RequestNewRoomOfUnknownTypeWait"), [Outcome]() { return Outcome->bFired; }, 30.0f));
	AddCommand(new FPlayServAssertStep(this, [Outcome](FAutomationTestBase* T)
	{
		T->TestFalse(TEXT("an unknown room type is refused"), Outcome->bSuccess);
		T->TestEqual(TEXT("problem code is room_type_not_found"), Outcome->Error.ProblemCode, FString(TEXT("room_type_not_found")));
		T->TestTrue(TEXT("typed NotFound"), Outcome->Error.Code == EPlayServErrorCode::NotFound);
		T->TestEqual(TEXT("the SDK posted"), Outcome->Sent.Verb, FString(TEXT("POST")));
		T->TestTrue(TEXT("to :host"), Outcome->Sent.Path.EndsWith(TEXT(":host")));
		T->TestEqual(TEXT("with the host deadline as the total timeout"), Outcome->Sent.TimeoutSeconds, PlayServRoomsClientWire::HostTimeoutSeconds);
		T->TestEqual(TEXT("and as the idle timeout, replacing the engine's 30 s"), Outcome->Sent.ActivityTimeoutSeconds, PlayServRoomsClientWire::HostTimeoutSeconds);
		T->TestTrue(TEXT("a deadline past the platform's own 5 s + 90 s bound"), PlayServRoomsClientWire::HostTimeoutSeconds > 95.0f);
	}));

	// The platform mints room names; a body naming one is refused before any routing.
	TSharedPtr<FJsonObject> NamedBody = MakeShared<FJsonObject>();
	NamedBody->SetStringField(TEXT("room_name"), TEXT("my-room"));
	FWireResultPtr Named = MakeShared<FWireResult>();
	AddFireStep(this, TEXT("HostNamingTheRoom"), TEXT("POST"), TEXT("/rooms/blob-arena:host"), TEXT("player"), NamedBody, Named);
	AddCommand(new FPlayServAssertStep(this, [Named](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("400"), Named->Status, 400);
		T->TestEqual(TEXT("problem code is bad_request"), Named->Code(), FString(TEXT("bad_request")));
	}));

	// Attributes are one level deep, checked before any server is asked. The contract documents
	// this refusal as a 400; the platform serves 422, which is what is pinned (drift found 2026-09-25).
	TSharedPtr<FJsonObject> NestedBody = MakeShared<FJsonObject>();
	TSharedPtr<FJsonObject> Nested = MakeShared<FJsonObject>();
	Nested->SetObjectField(TEXT("track"), MakeShared<FJsonObject>());
	NestedBody->SetObjectField(TEXT("attributes"), Nested);
	FWireResultPtr Deep = MakeShared<FWireResult>();
	AddFireStep(this, TEXT("HostWithNestedAttributes"), TEXT("POST"), TEXT("/rooms/blob-arena:host"), TEXT("player"), NestedBody, Deep);
	AddCommand(new FPlayServAssertStep(this, [Deep](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("422"), Deep->Status, 422);
		T->TestEqual(TEXT("problem code is attributes_too_large"), Deep->Code(), FString(TEXT("attributes_too_large")));
	}));

	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Contract.RoomsUplinkHelloAck — the live `wss://…/uplink` handshake.
//
// BR-15 again, on the socket: every field the hosting half reads out of `uplink_hello_ack` is
// asserted against the real platform — the session token it puts on REST calls, the admission
// path it confirms, and the room configuration that decides whether a room may start at all.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServContractRoomsUplinkHelloTest,
	"PlayServ.Contract.RoomsUplinkHelloAck",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServContractRoomsUplinkHelloTest::RunTest(const FString& Parameters)
{
	TSharedPtr<FPlayServUplinkClient> Client = MakeShared<FPlayServUplinkClient>(
		FPlayServWebSocketUplinkTransport::MakeFactory(), []() { return FPlatformTime::Seconds(); });

	TSharedPtr<FPlayServUplinkAck> Ack = MakeShared<FPlayServUplinkAck>();
	TSharedPtr<bool> bReady = MakeShared<bool>(false);
	TSharedPtr<FString> Refusal = MakeShared<FString>();

	Client->OnReady.BindLambda([Ack, bReady](const FPlayServUplinkAck& Received, int32)
	{
		*Ack = Received;
		*bReady = true;
	});
	Client->OnRefused.BindLambda([Refusal](const FString& Reason, bool) { *Refusal = Reason; });

	TSharedPtr<bool> bStarted = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("OpenUplink"), [Client, bStarted]()
	{
		FPlayServUplinkHelloParams Hello;
		Hello.ExecutorSlug = TEXT("blob-arena");
		Hello.InstanceId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens).ToLower();
		Hello.Capabilities.Add(TEXT("admission_push"));
		Client->Start(PlayServRoomsPaths::UplinkUrl(UPlayServSettings::GetBaseURL()), ServerKey(), Hello);
		*bStarted = true;
	}, bStarted));

	AddCommand(new FPlayServPollStep(this, TEXT("AwaitHelloAck"), [Client, bReady, Refusal]()
	{
		// The client is clock-driven, so the poll is what advances its handshake deadline.
		Client->Tick(FPlatformTime::Seconds());
		return *bReady || !Refusal->IsEmpty();
	}, 20.0f));

	AddCommand(new FPlayServAssertStep(this, [Client, Ack, bReady, Refusal](FAutomationTestBase* T)
	{
		if (!*bReady)
		{
			T->AddError(FString::Printf(TEXT("the live uplink never acked the hello (%s)"),
				Refusal->IsEmpty() ? TEXT("no refusal reported") : **Refusal));
			Client->Stop();
			return;
		}
		T->TestFalse(TEXT("session_token is served — the REST bearer while the socket is up"), Ack->SessionToken.IsEmpty());
		T->TestTrue(TEXT("expires_in is served — the SDK re-hellos before it lapses"), Ack->ExpiresInSeconds > 0);
		T->TestEqual(TEXT("admission is `push`, because the hello declared admission_push"), Ack->Admission, FString(TEXT("push")));
		if (!T->TestTrue(TEXT("room_config is served — blob-arena is a room type"), Ack->bHasRoomConfig))
		{
			Client->Stop();
			return;
		}
		T->TestTrue(TEXT("capacity"), Ack->RoomConfig.Capacity > 0);
		T->TestTrue(TEXT("reservation_ttl_seconds"), Ack->RoomConfig.ReservationTtlSeconds > 0);
		T->TestTrue(TEXT("max_rooms"), Ack->RoomConfig.MaxRooms > 0);
		T->TestTrue(TEXT("version"), Ack->RoomConfig.Version > 0);
		// A room configuration must carry at least one automatic expiry (ADR-005 §10).
		T->TestTrue(TEXT("a lifetime or an idle timeout — an unbounded room is refused at the operator PUT"),
			Ack->RoomConfig.RoomLifetimeSeconds > 0 || Ack->RoomConfig.bHasIdleTimeout);
		Client->Stop();
	}));
	return true;
}

#endif // !UE_BUILD_SHIPPING
