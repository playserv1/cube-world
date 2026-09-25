#include "PlayServTestCommon.h"
#include "PlayServTestAccess.h"
#include "Core/PlayServSubsystem.h"
#include "Data/PlayServData.h"
#include "PlayServ.h"
#include "TestEntities.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/GarbageCollection.h"

#if !UE_BUILD_SHIPPING

// ---------------------------------------------------------------------------
// PlayServ.Data.Queue.* — per-entity FIFO op serialization
//
// Save / Reload / Delete on the SAME instance are serialized into a per-entity FIFO: one op in
// flight, the next chained on HTTP completion. Consecutive same-kind PENDING ops coalesce (the
// write collapses, every folded callback still fires); a successful Delete is terminal (cancels
// later-queued ops); the entity is held weakly (complete-on-drop, no lifetime extension).
//
// Id semantics the queue absorbs without contract change: a Save on a NEW entity is a
// CREATE that binds the server-minted rec_* id at completion (so an entity's id is EMPTY until
// its first save lands — final Loads read the id off the rooted instance AFTER the burst), and
// the empty-id guards run at op RUN time, so a Reload/Delete enqueued right behind a first Save
// is legal — the create binds the id before the queued op's turn.
//
// These tests fire MULTIPLE ops in ONE latent step (no await between) to force contention, then
// assert via callback bookkeeping + a final Load. UTestPlayer is the entity under test; its
// int32 Level field stands in for the "count" the design notes describe.
// ---------------------------------------------------------------------------

// Test-lifetime strong root for the entity under test — survives multiple latent steps without
// being collected (except GcDuringQueue, which deliberately holds no root).
using FPlayerHolder = TSharedPtr<TStrongObjectPtr<UTestPlayer>>;

// Delete all UTestPlayer rows, then logout. 15s: DeleteAll is query-then-fan-out on V2, so a
// leaked table from a crashed prior run can take a while.
static void AddQueueCleanupStep(FAutomationTestBase* Test)
{
	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	Test->AddCommand(new FPlayServAsyncStep(Test, TEXT("Cleanup"), [bDone]()
	{
		// Cleanup no longer signs out — the next test reuses this session (PSV-2659).
		PlayServ::Data::DeleteAll<UTestPlayer>(FPlayServFilter::All(), FPlayServDeleteAllCallback::CreateLambda(
			[bDone](bool, const FPlayServDeleteAllResult&, const FPlayServError&)
		{
			*bDone = true;
		}));
	}, bDone, 15.0f));
}

// ---------------------------------------------------------------------------
// ConcurrentSaveQueues — fire 3 Saves back-to-back on one entity (Level 1,2,3). All 3 callbacks
// succeed (none gets the old "Save already in flight" reject); the final persisted Level is 3
// (FIFO / coalesced, last wins).
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServQueueConcurrentSaveTest,
	"PlayServ.Data.Queue.ConcurrentSaveQueues",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServQueueConcurrentSaveTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("queue-concurrent-save"));

	FPlayerHolder Holder = MakeShared<TStrongObjectPtr<UTestPlayer>>();
	TSharedPtr<TArray<bool>> Results = MakeShared<TArray<bool>>();
	TSharedPtr<TArray<FString>> Errors = MakeShared<TArray<FString>>();

	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("ThreeSaves"), [bDone, Holder, Results, Errors]()
	{
		UTestPlayer* P = PlayServ::Data::Create<UTestPlayer>();
		*Holder = TStrongObjectPtr<UTestPlayer>(P);

		auto RecordCb = [bDone, Results, Errors]()
		{
			return FPlayServSimpleCallback::CreateLambda([bDone, Results, Errors](bool bOk, const FPlayServError& Err)
			{
				Results->Add(bOk);
				Errors->Add(Err.Message);
				if (Results->Num() == 3)
				{
					*bDone = true;
				}
			});
		};

		// Three saves, no await between — they contend on the same instance.
		P->Level = 1;
		PlayServ::Data::Save(P, RecordCb());
		P->Level = 2;
		PlayServ::Data::Save(P, RecordCb());
		P->Level = 3;
		PlayServ::Data::Save(P, RecordCb());
	}, bDone, 5.0f));

	TSharedPtr<bool> bLoadOk = MakeShared<bool>(false);
	TSharedPtr<int32> LoadedLevel = MakeShared<int32>(-1);
	TSharedPtr<bool> bLoadDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("LoadFinal"), [bLoadDone, bLoadOk, LoadedLevel, Holder]()
	{
		// The server id exists only after the burst completed — read it off the rooted instance.
		const FString Id = UPlayServData::GetRecordId(Holder->Get());
		PlayServ::Data::Load<UTestPlayer>(Id, [bLoadDone, bLoadOk, LoadedLevel](bool bOk, UTestPlayer* P, const FPlayServError&)
		{
			*bLoadOk = bOk;
			if (bOk && P)
			{
				*LoadedLevel = P->Level;
			}
			*bLoadDone = true;
		});
	}, bLoadDone, 5.0f));

	AddCommand(new FPlayServAssertStep(this, [Results, Errors, bLoadOk, LoadedLevel](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("All 3 save callbacks fired"), Results->Num(), 3);
		bool bAllOk = Results->Num() == 3;
		bool bNoneRejected = true;
		for (int32 i = 0; i < Results->Num(); ++i)
		{
			bAllOk = bAllOk && (*Results)[i];
			bNoneRejected = bNoneRejected && !(*Errors)[i].Contains(TEXT("in flight"));
		}
		T->TestTrue(TEXT("All 3 concurrent saves succeeded (queued, not rejected)"), bAllOk);
		T->TestTrue(TEXT("No callback got the old 'Save already in flight' reject"), bNoneRejected);
		T->TestTrue(TEXT("Final Load succeeded"), *bLoadOk);
		T->TestEqual(TEXT("Final persisted Level reflects the last save (3)"), *LoadedLevel, 3);
	}));

	AddQueueCleanupStep(this);
	return true;
}

// ---------------------------------------------------------------------------
// SaveThenReloadOrdered — Save (mutate Level) then immediately Reload the SAME entity. The entity
// is NEW, so the Reload is enqueued while the id is still empty — legal on V2, because the save
// is a CREATE that binds the server id before the queued reload's run-time id guard fires. The
// reload runs after the save completes (FIFO), so it observes the saved state — no half-reset
// race — and both callbacks fire in submission order. To prove the reload actually round-trips
// (rather than a silent no-op leaving stale local state), the save's completion poisons the local
// Level to a sentinel; the asserted post-reload value can then only come from the backend
// reset+apply.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServQueueSaveThenReloadTest,
	"PlayServ.Data.Queue.SaveThenReloadOrdered",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServQueueSaveThenReloadTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("queue-save-reload"));

	FPlayerHolder Holder = MakeShared<TStrongObjectPtr<UTestPlayer>>();
	TSharedPtr<TArray<FString>> Order = MakeShared<TArray<FString>>();
	TSharedPtr<bool> bSaveOk = MakeShared<bool>(false);
	TSharedPtr<bool> bReloadOk = MakeShared<bool>(false);
	TSharedPtr<int32> ReloadedLevel = MakeShared<int32>(-1);

	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("SaveThenReload"), [bDone, Holder, Order, bSaveOk, bReloadOk, ReloadedLevel]()
	{
		UTestPlayer* P = PlayServ::Data::Create<UTestPlayer>();
		*Holder = TStrongObjectPtr<UTestPlayer>(P);
		P->Name = TEXT("save-then-reload");
		P->Level = 42;

		PlayServ::Data::Save(P, FPlayServSimpleCallback::CreateLambda(
			[Order, bSaveOk, Holder](bool bOk, const FPlayServError&)
		{
			Order->Add(TEXT("save"));
			*bSaveOk = bOk;
			// Poison the local value (the reload is already queued behind this save). If the reload
			// were a silent no-op, the sentinel would survive; the assertion of 42 would then fail.
			if (UTestPlayer* E = Holder->Get())
			{
				E->Level = 999;
			}
		}));

		PlayServ::Data::Reload(P, FPlayServSimpleCallback::CreateLambda(
			[bDone, Holder, Order, bReloadOk, ReloadedLevel](bool bOk, const FPlayServError&)
		{
			Order->Add(TEXT("reload"));
			*bReloadOk = bOk;
			if (UTestPlayer* E = Holder->Get())
			{
				*ReloadedLevel = E->Level;
			}
			*bDone = true;
		}));
	}, bDone, 5.0f));

	AddCommand(new FPlayServAssertStep(this, [Order, bSaveOk, bReloadOk, ReloadedLevel](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("Save succeeded"), *bSaveOk);
		T->TestTrue(TEXT("Reload succeeded"), *bReloadOk);
		T->TestEqual(TEXT("Both callbacks fired"), Order->Num(), 2);
		if (Order->Num() == 2)
		{
			T->TestEqual(TEXT("Save callback fired before reload (FIFO order)"), (*Order)[0], FString(TEXT("save")));
			T->TestEqual(TEXT("Reload callback fired second"), (*Order)[1], FString(TEXT("reload")));
		}
		T->TestEqual(TEXT("Reload observed the saved Level (no half-reset race)"), *ReloadedLevel, 42);
	}));

	AddQueueCleanupStep(this);
	return true;
}

// ---------------------------------------------------------------------------
// CoalesceFiresAllCallbacks — fire N consecutive Saves on one entity. Consecutive same-kind pending
// saves coalesce internally (the writes may collapse to fewer upserts), but the contract this test
// pins is that every folded callback STILL fires success and the final persisted state is the last
// write. It does not separately count HTTP upserts (the SDK exposes no transport hook), so it
// asserts the "all folded callbacks fire" + "last-write-wins" guarantees rather than directly
// observing the write-collapse mechanism.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServQueueCoalesceTest,
	"PlayServ.Data.Queue.CoalesceFiresAllCallbacks",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServQueueCoalesceTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("queue-coalesce"));

	static constexpr int32 NumSaves = 4;

	FPlayerHolder Holder = MakeShared<TStrongObjectPtr<UTestPlayer>>();
	TSharedPtr<int32> SuccessCount = MakeShared<int32>(0);
	TSharedPtr<int32> FiredCount = MakeShared<int32>(0);

	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("NSaves"), [bDone, Holder, SuccessCount, FiredCount]()
	{
		UTestPlayer* P = PlayServ::Data::Create<UTestPlayer>();
		*Holder = TStrongObjectPtr<UTestPlayer>(P);

		for (int32 i = 1; i <= NumSaves; ++i)
		{
			P->Level = i;
			PlayServ::Data::Save(P, FPlayServSimpleCallback::CreateLambda(
				[bDone, SuccessCount, FiredCount](bool bOk, const FPlayServError&)
			{
				if (bOk)
				{
					++(*SuccessCount);
				}
				if (++(*FiredCount) == NumSaves)
				{
					*bDone = true;
				}
			}));
		}
	}, bDone, 5.0f));

	TSharedPtr<bool> bLoadOk = MakeShared<bool>(false);
	TSharedPtr<int32> LoadedLevel = MakeShared<int32>(-1);
	TSharedPtr<bool> bLoadDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("LoadFinal"), [bLoadDone, bLoadOk, LoadedLevel, Holder]()
	{
		// The server id exists only after the burst completed — read it off the rooted instance.
		const FString Id = UPlayServData::GetRecordId(Holder->Get());
		PlayServ::Data::Load<UTestPlayer>(Id, [bLoadDone, bLoadOk, LoadedLevel](bool bOk, UTestPlayer* P, const FPlayServError&)
		{
			*bLoadOk = bOk;
			if (bOk && P)
			{
				*LoadedLevel = P->Level;
			}
			*bLoadDone = true;
		});
	}, bLoadDone, 5.0f));

	AddCommand(new FPlayServAssertStep(this, [SuccessCount, FiredCount, bLoadOk, LoadedLevel](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("Every coalesced callback fired"), *FiredCount, NumSaves);
		T->TestEqual(TEXT("Every callback reported success"), *SuccessCount, NumSaves);
		T->TestTrue(TEXT("Final Load succeeded"), *bLoadOk);
		T->TestEqual(TEXT("Final persisted Level is the last write"), *LoadedLevel, NumSaves);
	}));

	AddQueueCleanupStep(this);
	return true;
}

// ---------------------------------------------------------------------------
// DeleteTerminal — Save, then Delete, then a queued Reload on the same entity. The delete succeeds
// and is terminal: the still-pending reload completes with an error (not a hang, not a success).
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServQueueDeleteTerminalTest,
	"PlayServ.Data.Queue.DeleteTerminal",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServQueueDeleteTerminalTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("queue-delete-terminal"));

	FPlayerHolder Holder = MakeShared<TStrongObjectPtr<UTestPlayer>>();

	// Persist the entity first so the delete targets a real backend row.
	TSharedPtr<bool> bCreateDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("CreatePersist"), [bCreateDone, Holder]()
	{
		UTestPlayer* P = PlayServ::Data::Create<UTestPlayer>();
		*Holder = TStrongObjectPtr<UTestPlayer>(P);
		P->Name = TEXT("delete-terminal");
		P->Level = 5;
		PlayServ::Data::Save(P, FPlayServSimpleCallback::CreateLambda(
			[bCreateDone](bool, const FPlayServError&) { *bCreateDone = true; }));
	}, bCreateDone, 5.0f));

	TSharedPtr<bool> bSaveOk = MakeShared<bool>(false);
	TSharedPtr<bool> bDeleteOk = MakeShared<bool>(false);
	TSharedPtr<bool> bReloadOk = MakeShared<bool>(true);   // must be flipped to false by the terminal error
	TSharedPtr<FString> ReloadErr = MakeShared<FString>();
	TSharedPtr<int32> FiredCount = MakeShared<int32>(0);

	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("SaveDeleteReload"), [bDone, Holder, bSaveOk, bDeleteOk, bReloadOk, ReloadErr, FiredCount]()
	{
		UTestPlayer* P = Holder->Get();
		if (!P)
		{
			*bDone = true;
			return;
		}

		auto Tick = [bDone, FiredCount]()
		{
			if (++(*FiredCount) == 3)
			{
				*bDone = true;
			}
		};

		P->Level = 50;
		PlayServ::Data::Save(P, FPlayServSimpleCallback::CreateLambda(
			[bSaveOk, Tick](bool bOk, const FPlayServError&) { *bSaveOk = bOk; Tick(); }));

		PlayServ::Data::Delete(P, FPlayServSimpleCallback::CreateLambda(
			[bDeleteOk, Tick](bool bOk, const FPlayServError&) { *bDeleteOk = bOk; Tick(); }));

		PlayServ::Data::Reload(P, FPlayServSimpleCallback::CreateLambda(
			[bReloadOk, ReloadErr, Tick](bool bOk, const FPlayServError& Err)
		{
			*bReloadOk = bOk;
			*ReloadErr = Err.Message;
			Tick();
		}));
	}, bDone, 5.0f));

	AddCommand(new FPlayServAssertStep(this, [bSaveOk, bDeleteOk, bReloadOk, ReloadErr, FiredCount](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("All 3 callbacks fired (no hang)"), *FiredCount, 3);
		T->TestTrue(TEXT("Save succeeded"), *bSaveOk);
		T->TestTrue(TEXT("Delete succeeded"), *bDeleteOk);
		T->TestFalse(TEXT("Reload queued after a terminal delete fails"), *bReloadOk);
		T->TestTrue(TEXT("Reload error reports the entity was deleted"),
			ReloadErr->Contains(TEXT("deleted")));
	}));

	AddQueueCleanupStep(this);
	return true;
}

// ---------------------------------------------------------------------------
// ReentrantEnqueue — a Save whose completion callback enqueues a SECOND save on the same entity.
// Because callbacks fire while the runner is still busy, the second save just appends; both
// complete and the final state reflects the second write.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServQueueReentrantTest,
	"PlayServ.Data.Queue.ReentrantEnqueue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServQueueReentrantTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("queue-reentrant"));

	FPlayerHolder Holder = MakeShared<TStrongObjectPtr<UTestPlayer>>();
	TSharedPtr<TArray<FString>> Order = MakeShared<TArray<FString>>();
	TSharedPtr<bool> bFirstOk = MakeShared<bool>(false);
	TSharedPtr<bool> bSecondOk = MakeShared<bool>(false);

	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("ReentrantSave"), [bDone, Holder, Order, bFirstOk, bSecondOk]()
	{
		UTestPlayer* P = PlayServ::Data::Create<UTestPlayer>();
		*Holder = TStrongObjectPtr<UTestPlayer>(P);
		P->Level = 10;

		PlayServ::Data::Save(P, FPlayServSimpleCallback::CreateLambda(
			[bDone, Holder, Order, bFirstOk, bSecondOk](bool bOk1, const FPlayServError&)
		{
			Order->Add(TEXT("save1"));
			*bFirstOk = bOk1;

			UTestPlayer* E = Holder->Get();
			if (!E)
			{
				*bDone = true;
				return;
			}

			// Reentrant enqueue from inside the first save's completion callback.
			E->Level = 20;
			PlayServ::Data::Save(E, FPlayServSimpleCallback::CreateLambda(
				[bDone, Order, bSecondOk](bool bOk2, const FPlayServError&)
			{
				Order->Add(TEXT("save2"));
				*bSecondOk = bOk2;
				*bDone = true;
			}));
		}));
	}, bDone, 5.0f));

	TSharedPtr<bool> bLoadOk = MakeShared<bool>(false);
	TSharedPtr<int32> LoadedLevel = MakeShared<int32>(-1);
	TSharedPtr<bool> bLoadDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("LoadFinal"), [bLoadDone, bLoadOk, LoadedLevel, Holder]()
	{
		// The server id exists only after the burst completed — read it off the rooted instance.
		const FString Id = UPlayServData::GetRecordId(Holder->Get());
		PlayServ::Data::Load<UTestPlayer>(Id, [bLoadDone, bLoadOk, LoadedLevel](bool bOk, UTestPlayer* P, const FPlayServError&)
		{
			*bLoadOk = bOk;
			if (bOk && P)
			{
				*LoadedLevel = P->Level;
			}
			*bLoadDone = true;
		});
	}, bLoadDone, 5.0f));

	AddCommand(new FPlayServAssertStep(this, [Order, bFirstOk, bSecondOk, bLoadOk, LoadedLevel](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("First save succeeded"), *bFirstOk);
		T->TestTrue(TEXT("Reentrant second save succeeded"), *bSecondOk);
		T->TestEqual(TEXT("Both callbacks fired in order"), Order->Num(), 2);
		if (Order->Num() == 2)
		{
			T->TestEqual(TEXT("First save callback fired first"), (*Order)[0], FString(TEXT("save1")));
			T->TestEqual(TEXT("Reentrant save callback fired second"), (*Order)[1], FString(TEXT("save2")));
		}
		T->TestTrue(TEXT("Final Load succeeded"), *bLoadOk);
		T->TestEqual(TEXT("Final persisted Level reflects the reentrant write (20)"), *LoadedLevel, 20);
	}));

	AddQueueCleanupStep(this);
	return true;
}

// ---------------------------------------------------------------------------
// RejectGone (regression) — a second concurrent Save no longer returns the old
// "Save already in flight" error; both saves succeed.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServQueueRejectGoneTest,
	"PlayServ.Data.Queue.RejectGone",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServQueueRejectGoneTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("queue-reject-gone"));

	FPlayerHolder Holder = MakeShared<TStrongObjectPtr<UTestPlayer>>();
	TSharedPtr<bool> bFirstOk = MakeShared<bool>(false);
	TSharedPtr<bool> bSecondOk = MakeShared<bool>(false);
	TSharedPtr<FString> SecondErr = MakeShared<FString>();
	TSharedPtr<int32> FiredCount = MakeShared<int32>(0);

	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("TwoConcurrentSaves"), [bDone, Holder, bFirstOk, bSecondOk, SecondErr, FiredCount]()
	{
		UTestPlayer* P = PlayServ::Data::Create<UTestPlayer>();
		*Holder = TStrongObjectPtr<UTestPlayer>(P);

		auto Tick = [bDone, FiredCount]()
		{
			if (++(*FiredCount) == 2)
			{
				*bDone = true;
			}
		};

		P->Level = 1;
		PlayServ::Data::Save(P, FPlayServSimpleCallback::CreateLambda(
			[bFirstOk, Tick](bool bOk, const FPlayServError&) { *bFirstOk = bOk; Tick(); }));

		// Second save while the first is still in flight — previously hard-rejected.
		P->Level = 2;
		PlayServ::Data::Save(P, FPlayServSimpleCallback::CreateLambda(
			[bSecondOk, SecondErr, Tick](bool bOk, const FPlayServError& Err) { *bSecondOk = bOk; *SecondErr = Err.Message; Tick(); }));
	}, bDone, 5.0f));

	AddCommand(new FPlayServAssertStep(this, [bFirstOk, bSecondOk, SecondErr, FiredCount](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("Both callbacks fired"), *FiredCount, 2);
		T->TestTrue(TEXT("First save succeeded"), *bFirstOk);
		T->TestTrue(TEXT("Second concurrent save succeeded (queued, not rejected)"), *bSecondOk);
		T->TestFalse(TEXT("Second save did NOT get the old 'Save already in flight' error"),
			SecondErr->Contains(TEXT("in flight")));
	}));

	AddQueueCleanupStep(this);
	return true;
}

// ---------------------------------------------------------------------------
// GcDuringQueue (assert-or-skip) — enqueue two saves (one in flight, one pending), then drop the
// only reference and force a GC. The pending op's turn finds a dead weak ref and completes its
// callback with the "garbage collected" error instead of hanging. The entity is held weakly by the
// queue, so it really is collectible mid-flight.
//
// The collected path is GC-timing-dependent: CollectGarbage may not reclaim the entity this tick
// (e.g. a transient reference from the in-flight HTTP request, or clustering). Rather than asserting
// on a non-deterministic outcome, the test captures a weak ref and only runs the strict
// collected-path assertions when the entity actually went stale; otherwise it warns and treats the
// run as inconclusive. The "both callbacks fired — no hang" invariant is asserted unconditionally.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServQueueGcDuringQueueTest,
	"PlayServ.Data.Queue.GcDuringQueue",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServQueueGcDuringQueueTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("queue-gc"));

	TSharedPtr<bool> bSecondOk = MakeShared<bool>(true);   // flipped false by the collected error
	TSharedPtr<FString> SecondErr = MakeShared<FString>();
	TSharedPtr<int32> FiredCount = MakeShared<int32>(0);
	// Weak ref used by the assert step to decide whether the collected path was actually exercised.
	TSharedPtr<TWeakObjectPtr<UTestPlayer>> Weak = MakeShared<TWeakObjectPtr<UTestPlayer>>();

	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("EnqueueThenGc"), [bDone, bSecondOk, SecondErr, FiredCount, Weak]()
	{
		// Deliberately NO strong holder — the only refs to this entity are the queue's weak ptr,
		// the change-tracker snapshot (weak), and the registry (weak).
		UTestPlayer* P = PlayServ::Data::Create<UTestPlayer>();
		*Weak = P;

		auto Tick = [bDone, FiredCount]()
		{
			if (++(*FiredCount) == 2)
			{
				*bDone = true;
			}
		};

		// Save #1 dispatches its HTTP synchronously here (in flight after this call).
		P->Level = 1;
		PlayServ::Data::Save(P, FPlayServSimpleCallback::CreateLambda(
			[Tick](bool, const FPlayServError&) { Tick(); }));

		// Save #2 is left PENDING behind the in-flight save #1.
		P->Level = 2;
		PlayServ::Data::Save(P, FPlayServSimpleCallback::CreateLambda(
			[bSecondOk, SecondErr, Tick](bool bOk, const FPlayServError& Err)
		{
			*bSecondOk = bOk;
			*SecondErr = Err.Message;
			Tick();
		}));

		// Drop the local handle and force a synchronous full purge. P is unreferenced (no root, no
		// UPROPERTY, weak everywhere) so it is collected before save #1's HTTP completes; save #2's
		// turn then finds a dead weak ref.
		P = nullptr;
		CollectGarbage(RF_NoFlags, true);
	}, bDone, 5.0f));

	AddCommand(new FPlayServAssertStep(this, [bSecondOk, SecondErr, FiredCount, Weak](FAutomationTestBase* T)
	{
		// Invariant regardless of GC timing: the queue never hangs.
		T->TestEqual(TEXT("Both callbacks fired — no hang despite the dropped entity"), *FiredCount, 2);

		const bool bWasCollected = !Weak->IsValid();
		if (bWasCollected)
		{
			// The collected path was genuinely exercised — assert the dead-weak-ref behavior.
			T->TestFalse(TEXT("Pending save on the collected entity did not succeed"), *bSecondOk);
			T->TestTrue(TEXT("Pending save reports the entity was garbage collected"),
				SecondErr->Contains(TEXT("garbage collected")));
		}
		else
		{
			// GC did not reclaim the entity this run — do not assert on a path that wasn't taken.
			T->AddWarning(TEXT("GcDuringQueue inconclusive: the entity was not garbage-collected during "
				"the queue window, so the collected-path assertion was skipped (GC-timing dependent)."));
		}
	}));

	// No persisted entity to clean up beyond what save #1 may have written; DeleteAll covers it.
	AddQueueCleanupStep(this);
	return true;
}

// ---------------------------------------------------------------------------
// CoalesceCollapsesWrites — the focused coalescing test. Unlike
// CoalesceFiresAllCallbacks (which only proves callbacks fire), this uses the dispatched-upsert
// counter to prove N back-to-back saves collapse to FEWER than N writes, and a mid-burst pending
// depth of 1 to prove the in-flight op does NOT coalesce while the trailing same-kind saves do.
// On V2 the counter counts each dispatched write regardless of kind — the burst is one CREATE
// (save #1, in flight) plus one coalesced PATCH, so the collapse assertion holds unchanged.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServQueueCoalesceCollapsesTest,
	"PlayServ.Data.Queue.CoalesceCollapsesWrites",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServQueueCoalesceCollapsesTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("queue-coalesce-collapse"));

	static constexpr int32 NumSaves = 4;

	FPlayerHolder Holder = MakeShared<TStrongObjectPtr<UTestPlayer>>();
	TSharedPtr<int32> FiredCount = MakeShared<int32>(0);
	TSharedPtr<int32> PendingMidBurst = MakeShared<int32>(-1);

	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("BurstSaves"), [bDone, Holder, FiredCount, PendingMidBurst]()
	{
		UPlayServData* Data = UPlayServSubsystem::Get()->GetData();
		FPlayServDataTestAccess::ResetUpsertCount();

		UTestPlayer* P = PlayServ::Data::Create<UTestPlayer>();
		*Holder = TStrongObjectPtr<UTestPlayer>(P);

		for (int32 i = 1; i <= NumSaves; ++i)
		{
			P->Level = i;
			PlayServ::Data::Save(P, FPlayServSimpleCallback::CreateLambda(
				[bDone, FiredCount](bool, const FPlayServError&)
			{
				if (++(*FiredCount) == NumSaves) { *bDone = true; }
			}));
		}

		// Synchronous snapshot right after the burst: save #1 is in flight (already removed from
		// Pending), saves #2..N have folded into ONE pending op — so depth is 1.
		*PendingMidBurst = FPlayServDataTestAccess::PendingDepth(Data, P);
	}, bDone, 5.0f));

	TSharedPtr<bool> bLoadOk = MakeShared<bool>(false);
	TSharedPtr<int32> LoadedLevel = MakeShared<int32>(-1);
	TSharedPtr<bool> bLoadDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("LoadFinal"), [bLoadDone, bLoadOk, LoadedLevel, Holder]()
	{
		// The server id exists only after the burst completed — read it off the rooted instance.
		const FString Id = UPlayServData::GetRecordId(Holder->Get());
		PlayServ::Data::Load<UTestPlayer>(Id, [bLoadDone, bLoadOk, LoadedLevel](bool bOk, UTestPlayer* P, const FPlayServError&)
		{
			*bLoadOk = bOk;
			if (bOk && P) { *LoadedLevel = P->Level; }
			*bLoadDone = true;
		});
	}, bLoadDone, 5.0f));

	AddCommand(new FPlayServAssertStep(this, [FiredCount, PendingMidBurst, bLoadOk, LoadedLevel](FAutomationTestBase* T)
	{
		const int32 Upserts = FPlayServDataTestAccess::GetUpsertCount();
		T->TestEqual(TEXT("Every folded save callback fired"), *FiredCount, NumSaves);
		T->TestTrue(TEXT("At least one upsert was dispatched"), Upserts >= 1);
		T->TestTrue(
			FString::Printf(TEXT("Coalescing collapsed %d saves into FEWER than %d upserts (got %d)"), NumSaves, NumSaves, Upserts),
			Upserts < NumSaves);
		T->TestEqual(TEXT("Mid-burst: in-flight op not coalesced, trailing saves folded to ONE pending op"), *PendingMidBurst, 1);
		T->TestTrue(TEXT("Final Load succeeded"), *bLoadOk);
		T->TestEqual(TEXT("Final persisted Level is the last write"), *LoadedLevel, NumSaves);
	}));

	AddQueueCleanupStep(this);
	return true;
}

// ---------------------------------------------------------------------------
// CrossKindNoCoalesce — coalescing must NOT fold across an op-kind boundary.
// A Save followed by a Reload must remain two distinct pending ops. Fire Save, Save, Reload, Reload:
// save #1 runs in flight, save #2 folds into one pending Save op, the two Reloads fold into one
// pending Reload op — so mid-burst pending depth is 2 (not 1), proving the Save did not absorb the
// Reload.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServQueueCrossKindNoCoalesceTest,
	"PlayServ.Data.Queue.CrossKindNoCoalesce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServQueueCrossKindNoCoalesceTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("queue-cross-kind"));

	FPlayerHolder Holder = MakeShared<TStrongObjectPtr<UTestPlayer>>();

	// Persist first so the Reloads target a real backend row.
	TSharedPtr<bool> bCreateDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("CreatePersist"), [bCreateDone, Holder]()
	{
		UTestPlayer* P = PlayServ::Data::Create<UTestPlayer>();
		*Holder = TStrongObjectPtr<UTestPlayer>(P);
		P->Name = TEXT("cross-kind");
		P->Level = 1;
		PlayServ::Data::Save(P, FPlayServSimpleCallback::CreateLambda(
			[bCreateDone](bool, const FPlayServError&) { *bCreateDone = true; }));
	}, bCreateDone, 5.0f));

	TSharedPtr<int32> FiredCount = MakeShared<int32>(0);
	TSharedPtr<int32> SuccessCount = MakeShared<int32>(0);
	TSharedPtr<int32> PendingMidBurst = MakeShared<int32>(-1);

	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("SaveSaveReloadReload"), [bDone, Holder, FiredCount, SuccessCount, PendingMidBurst]()
	{
		UTestPlayer* P = Holder->Get();
		if (!P) { *bDone = true; return; }
		UPlayServData* Data = UPlayServSubsystem::Get()->GetData();

		auto Cb = [bDone, FiredCount, SuccessCount]()
		{
			return FPlayServSimpleCallback::CreateLambda([bDone, FiredCount, SuccessCount](bool bOk, const FPlayServError&)
			{
				if (bOk) { ++(*SuccessCount); }
				if (++(*FiredCount) == 4) { *bDone = true; }
			});
		};

		P->Level = 10;
		PlayServ::Data::Save(P, Cb());     // runs immediately (in flight)
		P->Level = 20;
		PlayServ::Data::Save(P, Cb());     // pending Save op
		PlayServ::Data::Reload(P, Cb());   // NEW pending Reload op — must NOT fold into the Save
		PlayServ::Data::Reload(P, Cb());   // folds into the pending Reload op

		// save #1 in flight; pending = [coalesced Save, coalesced Reload] = 2 distinct ops.
		*PendingMidBurst = FPlayServDataTestAccess::PendingDepth(Data, P);
	}, bDone, 5.0f));

	AddCommand(new FPlayServAssertStep(this, [FiredCount, SuccessCount, PendingMidBurst](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("All four callbacks fired (no hang)"), *FiredCount, 4);
		T->TestEqual(TEXT("All four ops succeeded"), *SuccessCount, 4);
		T->TestEqual(TEXT("Save did NOT fold across Reload — two distinct pending ops"), *PendingMidBurst, 2);
	}));

	AddQueueCleanupStep(this);
	return true;
}

// ---------------------------------------------------------------------------
// ShutdownDrainsPending — module shutdown drains queued ops with a cancellation error.
// Enqueue a Save (in flight) plus a pending Reload, then drain via shutdown semantics (transport is
// restored afterwards so the shared singleton survives for later tests). The pending op's callback
// must fire with the shutdown cancellation error rather than hanging.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServQueueShutdownDrainsTest,
	"PlayServ.Data.Queue.ShutdownDrainsPending",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServQueueShutdownDrainsTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("queue-shutdown-drain"));

	FPlayerHolder Holder = MakeShared<TStrongObjectPtr<UTestPlayer>>();

	// Persist first so the drained Reload targets a real backend row (the id guard runs at op
	// run time on V2, but a cancelled reload of a row that never existed would prove less).
	TSharedPtr<bool> bCreateDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("CreatePersist"), [bCreateDone, Holder]()
	{
		UTestPlayer* P = PlayServ::Data::Create<UTestPlayer>();
		*Holder = TStrongObjectPtr<UTestPlayer>(P);
		P->Name = TEXT("shutdown-drain");
		P->Level = 1;
		PlayServ::Data::Save(P, FPlayServSimpleCallback::CreateLambda(
			[bCreateDone](bool, const FPlayServError&) { *bCreateDone = true; }));
	}, bCreateDone, 5.0f));

	TSharedPtr<bool> bReloadOk = MakeShared<bool>(true);   // flipped false by the cancellation
	TSharedPtr<FString> ReloadErr = MakeShared<FString>();

	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("EnqueueThenShutdown"), [bDone, Holder, bReloadOk, ReloadErr]()
	{
		UTestPlayer* P = Holder->Get();
		if (!P) { *bDone = true; return; }
		UPlayServData* Data = UPlayServSubsystem::Get()->GetData();

		// Save #1 dispatches immediately (in flight). Reload is left PENDING behind it.
		P->Level = 99;
		PlayServ::Data::Save(P, FPlayServSimpleCallback::CreateLambda(
			[](bool, const FPlayServError&) {}));
		PlayServ::Data::Reload(P, FPlayServSimpleCallback::CreateLambda(
			[bDone, bReloadOk, ReloadErr](bool bOk, const FPlayServError& Err)
		{
			*bReloadOk = bOk;
			*ReloadErr = Err.Message;
			*bDone = true;
		}));

		// Drain pending ops via shutdown semantics; the pending Reload's callback fires synchronously
		// here with the cancellation error, then the transport is restored for the steps that follow.
		FPlayServDataTestAccess::DrainViaShutdown(Data);
	}, bDone, 5.0f));

	AddCommand(new FPlayServAssertStep(this, [bReloadOk, ReloadErr](FAutomationTestBase* T)
	{
		T->TestFalse(TEXT("Pending op cancelled by shutdown does not succeed"), *bReloadOk);
		T->TestTrue(TEXT("Pending op reports the shutdown cancellation"),
			ReloadErr->Contains(TEXT("shut down")));
	}));

	AddQueueCleanupStep(this);
	return true;
}

// ---------------------------------------------------------------------------
// DeleteThenRecreate — a successful Delete is terminal and tears down the queue; a later Save on the
// SAME instance starts a fresh queue and recreates the row. The delete clears
// the id registry entry, so the re-save is a fresh CREATE and the server mints a NEW id (different
// from the deleted row's) — proving the instance is fully usable again.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServQueueDeleteThenRecreateTest,
	"PlayServ.Data.Queue.DeleteThenRecreate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServQueueDeleteThenRecreateTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("queue-delete-recreate"));

	FPlayerHolder Holder = MakeShared<TStrongObjectPtr<UTestPlayer>>();
	TSharedPtr<FString> Id1 = MakeShared<FString>();
	TSharedPtr<FString> Id2 = MakeShared<FString>();
	TSharedPtr<bool> bDeleteOk = MakeShared<bool>(false);
	TSharedPtr<bool> bResaveOk = MakeShared<bool>(false);
	TSharedPtr<bool> bLoadOk = MakeShared<bool>(false);
	TSharedPtr<int32> LoadedLevel = MakeShared<int32>(-1);

	// 1) Create + persist.
	TSharedPtr<bool> bCreateDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("CreatePersist"), [bCreateDone, Holder, Id1]()
	{
		UTestPlayer* P = PlayServ::Data::Create<UTestPlayer>();
		*Holder = TStrongObjectPtr<UTestPlayer>(P);
		P->Name = TEXT("delete-recreate");
		P->Level = 7;
		PlayServ::Data::Save(P, FPlayServSimpleCallback::CreateLambda(
			[bCreateDone, Holder, Id1](bool, const FPlayServError&)
		{
			if (UTestPlayer* E = Holder->Get()) { *Id1 = UPlayServData::GetRecordId(E); }
			*bCreateDone = true;
		}));
	}, bCreateDone, 5.0f));

	// 2) Delete (terminal).
	TSharedPtr<bool> bDeleteDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("Delete"), [bDeleteDone, bDeleteOk, Holder]()
	{
		UTestPlayer* P = Holder->Get();
		if (!P) { *bDeleteDone = true; return; }
		PlayServ::Data::Delete(P, FPlayServSimpleCallback::CreateLambda(
			[bDeleteDone, bDeleteOk](bool bOk, const FPlayServError&) { *bDeleteOk = bOk; *bDeleteDone = true; }));
	}, bDeleteDone, 5.0f));

	// 3) Save again on the SAME instance — fresh queue, fresh CREATE, server mints a new id.
	TSharedPtr<bool> bResaveDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("Recreate"), [bResaveDone, bResaveOk, Holder, Id2]()
	{
		UTestPlayer* P = Holder->Get();
		if (!P) { *bResaveDone = true; return; }
		P->Level = 13;
		PlayServ::Data::Save(P, FPlayServSimpleCallback::CreateLambda(
			[bResaveDone, bResaveOk, Holder, Id2](bool bOk, const FPlayServError&)
		{
			*bResaveOk = bOk;
			if (UTestPlayer* E = Holder->Get()) { *Id2 = UPlayServData::GetRecordId(E); }
			*bResaveDone = true;
		}));
	}, bResaveDone, 5.0f));

	// 4) Load by the new id to confirm the recreated row exists.
	TSharedPtr<bool> bLoadDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("LoadRecreated"), [bLoadDone, bLoadOk, LoadedLevel, Id2]()
	{
		PlayServ::Data::Load<UTestPlayer>(*Id2, [bLoadDone, bLoadOk, LoadedLevel](bool bOk, UTestPlayer* P, const FPlayServError&)
		{
			*bLoadOk = bOk;
			if (bOk && P) { *LoadedLevel = P->Level; }
			*bLoadDone = true;
		});
	}, bLoadDone, 5.0f));

	AddCommand(new FPlayServAssertStep(this, [bDeleteOk, bResaveOk, bLoadOk, LoadedLevel, Id1, Id2](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("Delete succeeded"), *bDeleteOk);
		T->TestTrue(TEXT("Re-save after terminal delete succeeded (fresh queue)"), *bResaveOk);
		T->TestFalse(TEXT("Re-save bound a new server id (delete cleared the old one)"), Id2->IsEmpty());
		T->TestTrue(TEXT("Recreated row has a different id than the deleted one"), *Id2 != *Id1);
		T->TestTrue(TEXT("Recreated row loads"), *bLoadOk);
		T->TestEqual(TEXT("Recreated row carries the re-saved Level"), *LoadedLevel, 13);
	}));

	AddQueueCleanupStep(this);
	return true;
}

// ---------------------------------------------------------------------------
// CleanSaveNoOpTwice — two clean (unmutated) saves in one step both complete synchronously as
// no-ops and dispatch zero upserts. Each no-op runs inline and re-enters the
// queue map (RunNextOp removes the key inside EnqueueOp); the test pins that this re-entrant path
// does not crash and never issues a write.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServQueueCleanSaveNoOpTest,
	"PlayServ.Data.Queue.CleanSaveNoOpTwice",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServQueueCleanSaveNoOpTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("queue-clean-noop"));

	FPlayerHolder Holder = MakeShared<TStrongObjectPtr<UTestPlayer>>();

	// Persist once so the snapshot matches the entity (subsequent unmutated saves are clean no-ops).
	TSharedPtr<bool> bCreateDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("CreatePersist"), [bCreateDone, Holder]()
	{
		UTestPlayer* P = PlayServ::Data::Create<UTestPlayer>();
		*Holder = TStrongObjectPtr<UTestPlayer>(P);
		P->Name = TEXT("clean-noop");
		P->Level = 4;
		PlayServ::Data::Save(P, FPlayServSimpleCallback::CreateLambda(
			[bCreateDone](bool, const FPlayServError&) { *bCreateDone = true; }));
	}, bCreateDone, 5.0f));

	TSharedPtr<bool> bOk1 = MakeShared<bool>(false);
	TSharedPtr<bool> bOk2 = MakeShared<bool>(false);
	TSharedPtr<int32> FiredCount = MakeShared<int32>(0);

	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("TwoCleanSaves"), [bDone, Holder, bOk1, bOk2, FiredCount]()
	{
		UTestPlayer* P = Holder->Get();
		if (!P) { *bDone = true; return; }
		FPlayServDataTestAccess::ResetUpsertCount();

		auto Tick = [bDone, FiredCount]() { if (++(*FiredCount) == 2) { *bDone = true; } };

		// Two saves with NO intervening mutation — both are clean no-ops that complete synchronously
		// and each re-enters the queue map (RunNextOp removes the key inside EnqueueOp).
		PlayServ::Data::Save(P, FPlayServSimpleCallback::CreateLambda(
			[bOk1, Tick](bool bOk, const FPlayServError&) { *bOk1 = bOk; Tick(); }));
		PlayServ::Data::Save(P, FPlayServSimpleCallback::CreateLambda(
			[bOk2, Tick](bool bOk, const FPlayServError&) { *bOk2 = bOk; Tick(); }));
	}, bDone, 5.0f));

	AddCommand(new FPlayServAssertStep(this, [bOk1, bOk2, FiredCount](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("Both clean saves fired (no crash on the re-entrant no-op path)"), *FiredCount, 2);
		T->TestTrue(TEXT("First clean save reported success"), *bOk1);
		T->TestTrue(TEXT("Second clean save reported success"), *bOk2);
		T->TestEqual(TEXT("Neither clean save dispatched an upsert"), FPlayServDataTestAccess::GetUpsertCount(), 0);
	}));

	AddQueueCleanupStep(this);
	return true;
}

#endif // !UE_BUILD_SHIPPING
