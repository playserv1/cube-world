#include "PlayServTestCommon.h"
#include "PlayServTestAccess.h"
#include "Core/PlayServSubsystem.h"
#include "Data/PlayServData.h"
#include "PlayServ.h"
#include "TestEntities.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

#if !UE_BUILD_SHIPPING

// ---------------------------------------------------------------------------
// PlayServ.Data.Reload.* — reload overwrite semantics
//
// ReloadEntity is a backend-wins overwrite: reset to CDO, hydrate from the server document,
// re-snapshot. These tests pin that contract directly on field values.
//
// Change notification lives with the subscription tests (PlayServ.Data.Subscribe.*); what is
// pinned here is the overwrite itself and FP round-trip stability.
//
// To produce a server-side change, a test loads a SECOND instance of the same entity (which
// gives it a snapshot), mutates it, and Saves — the change tracker turns that into a delta
// upsert touching only the mutated fields. The ORIGINAL instance is then reloaded.
// ---------------------------------------------------------------------------

using FNotifyHolder = TSharedPtr<TStrongObjectPtr<UTestNotifyEntity>>;

// Create the original entity, root it in the holder, record its ID, and Save it.
static void AddCreateOriginalStep(FAutomationTestBase* Test, FNotifyHolder Original, TSharedPtr<FString> Id,
	TFunction<void(UTestNotifyEntity*)> Init)
{
	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	Test->AddCommand(new FPlayServAsyncStep(Test, TEXT("CreateOriginal"), [bDone, Original, Id, Init]()
	{
		UTestNotifyEntity* Entity = PlayServ::Data::Create<UTestNotifyEntity>();
		Init(Entity);
		*Original = TStrongObjectPtr<UTestNotifyEntity>(Entity);

		// The id is server-minted: capture it in the SAVE callback (empty before the create
		// response binds it).
		PlayServ::Data::Save(Entity, FPlayServSimpleCallback::CreateLambda(
			[bDone, Id, Original](bool, const FPlayServError&)
			{
				if (Original->IsValid())
				{
					*Id = UPlayServData::GetRecordId(Original->Get());
				}
				*bDone = true;
			}));
	}, bDone, 8.0f));
}

// Out-of-band server mutation: load a fresh copy (gets a snapshot), apply the mutator, Save the
// delta. The original instance's local state is untouched.
static void AddServerMutateStep(FAutomationTestBase* Test, TSharedPtr<FString> Id,
	TFunction<void(UTestNotifyEntity*)> Mutator)
{
	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	Test->AddCommand(new FPlayServAsyncStep(Test, TEXT("ServerMutate"), [bDone, Id, Mutator]()
	{
		PlayServ::Data::Load<UTestNotifyEntity>(*Id,
			[bDone, Mutator](bool bLoad, UTestNotifyEntity* Other, const FPlayServError&)
		{
			if (!bLoad || !Other)
			{
				*bDone = true;
				return;
			}
			Mutator(Other);
			PlayServ::Data::Save(Other, FPlayServSimpleCallback::CreateLambda(
				[bDone](bool, const FPlayServError&) { *bDone = true; }));
		});
	}, bDone, 3.0f));
}

// Reload the original instance.
static void AddReloadStep(FAutomationTestBase* Test, FNotifyHolder Original)
{
	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	Test->AddCommand(new FPlayServAsyncStep(Test, TEXT("Reload"), [bDone, Original]()
	{
		UTestNotifyEntity* Entity = Original->Get();
		if (!Entity)
		{
			*bDone = true;
			return;
		}
		PlayServ::Data::Reload(Entity, FPlayServSimpleCallback::CreateLambda(
			[bDone](bool, const FPlayServError&) { *bDone = true; }));
	}, bDone, 3.0f));
}

// Delete the entity server-side and release the holder root.
static void AddCleanupStep(FAutomationTestBase* Test, FNotifyHolder Original)
{
	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	Test->AddCommand(new FPlayServAsyncStep(Test, TEXT("Cleanup"), [bDone, Original]()
	{
		UTestNotifyEntity* Entity = Original->Get();
		if (!Entity)
		{
			*bDone = true;
			return;
		}
		PlayServ::Data::Delete(Entity, FPlayServSimpleCallback::CreateLambda(
			[bDone, Original](bool, const FPlayServError&)
			{
				Original->Reset();
				*bDone = true;
			}));
	}, bDone, 3.0f));
}

// ---------------------------------------------------------------------------
// OverwritesLocalState — a reload after an out-of-band server change overwrites the local
// fields with server state (backend-wins), including a locally-mutated field the server did
// not touch (overwrite, not merge).
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServReloadOverwritesLocalStateTest,
	"PlayServ.Data.Reload.OverwritesLocalState",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServReloadOverwritesLocalStateTest::RunTest(const FString& Parameters)
{
	FNotifyHolder Original = MakeShared<TStrongObjectPtr<UTestNotifyEntity>>();
	TSharedPtr<FString> Id = MakeShared<FString>();

	AddLoginStep(this, TEXT("reload-overwrite-test"));
	AddCreateOriginalStep(this, Original, Id, [](UTestNotifyEntity* E)
	{
		E->Title = TEXT("initial");
		E->Count = 1;
	});

	// Server-side: Title changes. Locally (below): Count is dirtied but NOT saved.
	AddServerMutateStep(this, Id, [](UTestNotifyEntity* Other)
	{
		Other->Title = TEXT("server-changed");
	});

	TSharedPtr<bool> bMutated = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("LocalDirty"), [Original, bMutated]()
	{
		if (UTestNotifyEntity* Entity = Original->Get())
		{
			Entity->Count = 999;   // unsaved local edit — the overwrite must discard it
		}
		*bMutated = true;
	}, bMutated));

	AddReloadStep(this, Original);

	AddCommand(new FPlayServAssertStep(this, [Original](FAutomationTestBase* Test)
	{
		UTestNotifyEntity* Entity = Original->Get();
		if (Entity == nullptr)
		{
			Test->AddError(TEXT("Entity lost during test"));
			return;
		}
		Test->TestEqual(TEXT("Server-changed field overwritten onto local instance"),
			Entity->Title, FString(TEXT("server-changed")));
		Test->TestEqual(TEXT("Unsaved local edit discarded by the overwrite (backend-wins)"),
			Entity->Count, 1);
	}));

	AddCleanupStep(this, Original);
	return true;
}

// ---------------------------------------------------------------------------
// NoOpFloatRoundTrip — save then reload with no server-side change: float/double fields must
// round-trip exactly (guards JSON FP drift, the likeliest source of spurious diffs for the
// Phase-6 notify rebuild).
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServReloadNoOpFloatRoundTripTest,
	"PlayServ.Data.Reload.NoOpFloatRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServReloadNoOpFloatRoundTripTest::RunTest(const FString& Parameters)
{
	FNotifyHolder Original = MakeShared<TStrongObjectPtr<UTestNotifyEntity>>();
	TSharedPtr<FString> Id = MakeShared<FString>();

	AddLoginStep(this, TEXT("reload-noop-test"));
	AddCreateOriginalStep(this, Original, Id, [](UTestNotifyEntity* E)
	{
		E->Title = TEXT("noop");
		E->Ratio = 0.3f;        // not exactly representable — the drift-prone case
		E->Precise = 1.0 / 3.0;
	});

	AddReloadStep(this, Original);

	AddCommand(new FPlayServAssertStep(this, [Original](FAutomationTestBase* Test)
	{
		UTestNotifyEntity* Entity = Original->Get();
		if (Entity == nullptr)
		{
			Test->AddError(TEXT("Entity lost during test"));
			return;
		}
		Test->TestEqual(TEXT("float survives the save/reload round-trip exactly"), Entity->Ratio, 0.3f);
		Test->TestEqual(TEXT("double survives the save/reload round-trip exactly"), Entity->Precise, 1.0 / 3.0);
		Test->TestEqual(TEXT("string untouched"), Entity->Title, FString(TEXT("noop")));
	}));

	AddCleanupStep(this, Original);
	return true;
}

#endif // !UE_BUILD_SHIPPING
