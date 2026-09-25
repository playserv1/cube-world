#include "PlayServTestCommon.h"
#include "PlayServTestAccess.h"
#include "Core/PlayServSubsystem.h"
#include "Data/PlayServData.h"
#include "PlayServ.h"
// Private header, same plugin: the row-deletion test asserts the wire signal itself (the
// platform-sent Terminated frame), which no public surface exposes. Same precedent as
// PlayServ.Auth.V2.LoginSendsNoPlayerBearer reaching PlayServHttp.h.
#include "Realtime/PlayServDataflow.h"
#include "TestEntities.h"
#include "UObject/StrongObjectPtr.h"

#if !UE_BUILD_SHIPPING

// ---------------------------------------------------------------------------
// PlayServ.Data.Subscribe.* — entity subscriptions over the WS dataflow plane.
//
// Live against dev: a subscription requires a logged-in CLIENT session (the dataflow plane
// refuses pk_-only admission), and pushes arrive asynchronously — the receive tests poll with
// generous timeouts. Out-of-band changes are produced the reload-test way: load a SECOND
// instance of the same record, mutate, Save (merge patch) — the platform then pushes the new
// document to the subscribed first instance.
// ---------------------------------------------------------------------------

namespace
{
	using FSubHolder = TSharedPtr<TStrongObjectPtr<UTestNotifyEntity>>;

	// Create + save the subscribed-to entity; capture the server-minted id from the Save callback.
	void AddCreateSubjectStep(FAutomationTestBase* Test, FSubHolder Holder, TSharedPtr<FString> Id,
		TFunction<void(UTestNotifyEntity*)> Init)
	{
		TSharedPtr<bool> bDone = MakeShared<bool>(false);
		Test->AddCommand(new FPlayServAsyncStep(Test, TEXT("CreateSubject"), [bDone, Holder, Id, Init]()
		{
			UTestNotifyEntity* Entity = PlayServ::Data::Create<UTestNotifyEntity>();
			Init(Entity);
			*Holder = TStrongObjectPtr<UTestNotifyEntity>(Entity);
			PlayServ::Data::Save(Entity, FPlayServSimpleCallback::CreateLambda(
				[bDone, Id, Holder](bool, const FPlayServError&)
				{
					if (Holder->IsValid())
					{
						*Id = UPlayServData::GetRecordId(Holder->Get());
					}
					*bDone = true;
				}));
		}, bDone, 10.0f));
	}

	// Out-of-band server-side mutation via a second instance (same pattern as the Reload tests).
	void AddOutOfBandMutateStep(FAutomationTestBase* Test, TSharedPtr<FString> Id,
		TFunction<void(UTestNotifyEntity*)> Mutator)
	{
		TSharedPtr<bool> bDone = MakeShared<bool>(false);
		Test->AddCommand(new FPlayServAsyncStep(Test, TEXT("OutOfBandMutate"), [bDone, Id, Mutator]()
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
		}, bDone, 10.0f));
	}

	// Delete the subject row + logout.
	void AddSubscribeCleanupStep(FAutomationTestBase* Test, FSubHolder Holder)
	{
		TSharedPtr<bool> bDone = MakeShared<bool>(false);
		Test->AddCommand(new FPlayServAsyncStep(Test, TEXT("Cleanup"), [bDone, Holder]()
		{
			// Releases the handle but keeps the session: the next test reuses it rather than
			// minting another player (PSV-2659).
			auto Finish = [bDone, Holder]()
			{
				Holder->Reset();
				*bDone = true;
			};
			if (!Holder->IsValid() || UPlayServData::GetRecordId(Holder->Get()).IsEmpty())
			{
				Finish();
				return;
			}
			PlayServ::Data::Delete(Holder->Get(), FPlayServSimpleCallback::CreateLambda(
				[Finish](bool, const FPlayServError&) { Finish(); }));
		}, bDone, 10.0f));
	}

	int32 LiveHandleCount()
	{
		UPlayServSubsystem* PS = UPlayServSubsystem::Get();
		return PS ? FPlayServDataTestAccess::SubscriptionHandleCount(PS->GetData()) : -1;
	}

	// Subscribe() only QUEUES registration — the socket may still be dialing when it returns.
	// A test that mutates or deletes the subject before the platform has confirmed the
	// registration races its own subscription and sees nothing. (The retired collection
	// workaround hid this: it synthesised a deletion from the first frame's contents, so a
	// delete that landed before registration still produced a 49001.)
	void AddWaitForRegistrationStep(FAutomationTestBase* Test, int32 ExpectedRows)
	{
		Test->AddCommand(new FPlayServPollStep(Test, TEXT("WaitForRegistration"), [ExpectedRows]()
		{
			return FPlayServDataflow::TestRegisteredRowCount >= ExpectedRows;
		}, 20.0f));
	}

	void ResetDataflowWireCounters()
	{
		FPlayServDataflow::TestRegisteredRowCount = 0;
		FPlayServDataflow::TestTerminatedFrameCount = 0;
		FPlayServDataflow::TestLastTerminatedCode = 0;
	}
}

// ---------------------------------------------------------------------------
// RejectsInvalid — every requirement violation returns an INVALID handle and registers nothing:
// null entity, unsaved entity (no server id), and no client session.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServSubscribeRejectsInvalidTest,
	"PlayServ.Data.Subscribe.RejectsInvalid",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServSubscribeRejectsInvalidTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));

	// No session yet: a valid-looking entity must still be rejected.
	TSharedPtr<bool> bLoggedOut = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("EnsureLoggedOut"), [bLoggedOut]()
	{
		if (PlayServ::Auth::IsLoggedIn())
		{
			PlayServ::Auth::Logout(FPlayServSimpleCallback::CreateLambda(
				[bLoggedOut](bool, const FPlayServError&) { *bLoggedOut = true; }));
		}
		else
		{
			*bLoggedOut = true;
		}
	}, bLoggedOut));

	AddCommand(new FPlayServAssertStep(this, [](FAutomationTestBase* T)
	{
		UTestNotifyEntity* Unsaved = PlayServ::Data::Create<UTestNotifyEntity>();
		FPlayServDataTestAccess::BindRecordId(Unsaved, TEXT("rec_00000000000000000000000000"));
		FPlayServSubscriptionHandle NoSession = PlayServ::Data::Subscribe(Unsaved);
		T->TestFalse(TEXT("no client session -> invalid handle"), NoSession.IsValid());
	}));

	AddLoginStep(this, TEXT("subscribe-invalid-test"));

	AddCommand(new FPlayServAssertStep(this, [](FAutomationTestBase* T)
	{
		FPlayServSubscriptionHandle NullHandle = PlayServ::Data::Subscribe(nullptr);
		T->TestFalse(TEXT("null entity -> invalid handle"), NullHandle.IsValid());

		UTestNotifyEntity* Unsaved = PlayServ::Data::Create<UTestNotifyEntity>();
		FPlayServSubscriptionHandle UnsavedHandle = PlayServ::Data::Subscribe(Unsaved);
		T->TestFalse(TEXT("id-less (never saved) entity -> invalid handle"), UnsavedHandle.IsValid());

		T->TestEqual(TEXT("nothing registered"), LiveHandleCount(), 0);

		// Unsubscribe on an invalid handle is a safe no-op.
		PlayServ::Data::Unsubscribe(FPlayServSubscriptionHandle());
	}));

	return true;
}

// ---------------------------------------------------------------------------
// ReceivesOutOfBandChange — the core contract: an out-of-band server change is pushed to the
// subscribed instance (backend-wins overwrite) and the delegate reports the changed top-level
// field names.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServSubscribeReceivesOutOfBandChangeTest,
	"PlayServ.Data.Subscribe.ReceivesOutOfBandChange",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServSubscribeReceivesOutOfBandChangeTest::RunTest(const FString& Parameters)
{
	FSubHolder Holder = MakeShared<TStrongObjectPtr<UTestNotifyEntity>>();
	TSharedPtr<FString> Id = MakeShared<FString>();
	TSharedPtr<FPlayServSubscriptionHandle> Handle = MakeShared<FPlayServSubscriptionHandle>();
	TSharedPtr<bool> bNotified = MakeShared<bool>(false);
	TSharedPtr<TArray<FName>> Changed = MakeShared<TArray<FName>>();

	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("subscribe-receive-test"));
	AddCreateSubjectStep(this, Holder, Id, [](UTestNotifyEntity* E)
	{
		E->Title = TEXT("initial");
		E->Count = 1;
	});

	TSharedPtr<bool> bSubscribed = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("Subscribe"), [bSubscribed, Holder, Handle, bNotified, Changed]()
	{
		ResetDataflowWireCounters();
		if (Holder->IsValid())
		{
			*Handle = PlayServ::Data::Subscribe(Holder->Get(),
				FOnPlayServObjectChanged::CreateLambda([bNotified, Changed](UObject*, const TArray<FName>& InChanged)
				{
					*Changed = InChanged;
					*bNotified = true;
				}));
		}
		*bSubscribed = true;
	}, bSubscribed));

	AddCommand(new FPlayServAssertStep(this, [Handle](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("subscribe returned a valid handle"), Handle->IsValid());
	}));

	AddWaitForRegistrationStep(this, 1);

	AddOutOfBandMutateStep(this, Id, [](UTestNotifyEntity* Other)
	{
		Other->Title = TEXT("live-changed");
	});

	// The push is asynchronous — poll generously.
	AddCommand(new FPlayServPollStep(this, TEXT("WaitForPush"), [bNotified]() { return *bNotified; }, 20.0f));

	AddCommand(new FPlayServAssertStep(this, [Holder, Changed, bNotified](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("delegate fired"), *bNotified);
		if (UTestNotifyEntity* Entity = Holder->Get())
		{
			T->TestEqual(TEXT("push overwrote the subscribed instance (backend-wins)"),
				Entity->Title, FString(TEXT("live-changed")));
			T->TestEqual(TEXT("untouched field survived the overwrite"), Entity->Count, 1);
		}
		else
		{
			T->AddError(TEXT("subject entity lost"));
		}
		T->TestTrue(TEXT("changed-fields names the mutated top-level field"), Changed->Contains(FName(TEXT("Title"))));
		T->TestFalse(TEXT("changed-fields does NOT name the untouched field"), Changed->Contains(FName(TEXT("Count"))));
	}));

	TSharedPtr<bool> bUnsubscribed = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("Unsubscribe"), [bUnsubscribed, Handle]()
	{
		PlayServ::Data::Unsubscribe(*Handle);
		*bUnsubscribed = true;
	}, bUnsubscribed));

	AddSubscribeCleanupStep(this, Holder);
	return true;
}

// ---------------------------------------------------------------------------
// ServiceRestartResumes — a platform deploy drains the old revision by closing every player socket with 1012
// (Service Restart). The client resumes on the same session, re-registers its rows, and the subscription keeps
// receiving pushes: no handle is dropped.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServSubscribeServiceRestartResumesTest,
	"PlayServ.Data.Subscribe.ServiceRestartResumes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServSubscribeServiceRestartResumesTest::RunTest(const FString& Parameters)
{
	FSubHolder Holder = MakeShared<TStrongObjectPtr<UTestNotifyEntity>>();
	TSharedPtr<FString> Id = MakeShared<FString>();
	TSharedPtr<FPlayServSubscriptionHandle> Handle = MakeShared<FPlayServSubscriptionHandle>();
	TSharedPtr<bool> bNotified = MakeShared<bool>(false);
	TSharedPtr<TArray<FName>> Changed = MakeShared<TArray<FName>>();

	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("subscribe-service-restart-test"));
	AddCreateSubjectStep(this, Holder, Id, [](UTestNotifyEntity* E)
	{
		E->Title = TEXT("before restart");
	});

	TSharedPtr<bool> bSubscribed = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("Subscribe"), [bSubscribed, Holder, Handle, bNotified, Changed]()
	{
		ResetDataflowWireCounters();
		*Handle = PlayServ::Data::Subscribe(Holder->Get(), FOnPlayServObjectChanged::CreateLambda([bNotified, Changed](UObject*, const TArray<FName>& InChanged)
		{
			*Changed = InChanged;
			*bNotified = true;
		}));
		*bSubscribed = true;
	}, bSubscribed));
	AddWaitForRegistrationStep(this, 1);

	AddCommand(new FPlayServAssertStep(this, [](FAutomationTestBase* T)
	{
		ResetDataflowWireCounters();
		UPlayServSubsystem* PS = UPlayServSubsystem::Get();
		T->TestTrue(TEXT("the realtime socket was open to close"), PS != nullptr && FPlayServDataTestAccess::SimulateRealtimeClose(PS->GetData(), 1012));
		T->TestEqual(TEXT("the close drops no handle"), LiveHandleCount(), 1);
	}));

	AddWaitForRegistrationStep(this, 1);
	AddCommand(new FPlayServDelayStep(1.0f));
	AddCommand(new FPlayServAssertStep(this, [bNotified](FAutomationTestBase*) { *bNotified = false; }));

	AddOutOfBandMutateStep(this, Id, [](UTestNotifyEntity* Other)
	{
		Other->Title = TEXT("after restart");
	});
	AddCommand(new FPlayServPollStep(this, TEXT("WaitForPushAfterRestart"), [bNotified]() { return *bNotified; }, 20.0f));
	AddCommand(new FPlayServAssertStep(this, [Holder, Changed](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("a push after the restart reaches the subscription"), Changed->Contains(FName(TEXT("Title"))));
		T->TestEqual(TEXT("and overwrites the instance"), Holder->Get()->Title, FString(TEXT("after restart")));
		T->TestEqual(TEXT("the handle survived"), LiveHandleCount(), 1);
	}));

	TSharedPtr<bool> bUnsubscribed = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("Unsubscribe"), [bUnsubscribed, Handle]()
	{
		PlayServ::Data::Unsubscribe(*Handle);
		*bUnsubscribed = true;
	}, bUnsubscribed));

	AddSubscribeCleanupStep(this, Holder);
	return true;
}

// ---------------------------------------------------------------------------
// SilentSubscribeKeepsCurrent — the no-delegate overload still applies pushes (keep-current).
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServSubscribeSilentKeepsCurrentTest,
	"PlayServ.Data.Subscribe.SilentKeepsCurrent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServSubscribeSilentKeepsCurrentTest::RunTest(const FString& Parameters)
{
	FSubHolder Holder = MakeShared<TStrongObjectPtr<UTestNotifyEntity>>();
	TSharedPtr<FString> Id = MakeShared<FString>();
	TSharedPtr<FPlayServSubscriptionHandle> Handle = MakeShared<FPlayServSubscriptionHandle>();

	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("subscribe-silent-test"));
	AddCreateSubjectStep(this, Holder, Id, [](UTestNotifyEntity* E)
	{
		E->Title = TEXT("silent-initial");
	});

	TSharedPtr<bool> bSubscribed = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("SilentSubscribe"), [bSubscribed, Holder, Handle]()
	{
		ResetDataflowWireCounters();
		if (Holder->IsValid())
		{
			*Handle = PlayServ::Data::Subscribe(Holder->Get());
		}
		*bSubscribed = true;
	}, bSubscribed));

	AddWaitForRegistrationStep(this, 1);

	AddOutOfBandMutateStep(this, Id, [](UTestNotifyEntity* Other)
	{
		Other->Title = TEXT("silent-changed");
	});

	AddCommand(new FPlayServPollStep(this, TEXT("WaitForSilentPush"), [Holder]()
	{
		UTestNotifyEntity* Entity = Holder->Get();
		return Entity != nullptr && Entity->Title == TEXT("silent-changed");
	}, 20.0f));

	AddCommand(new FPlayServAssertStep(this, [Holder, Handle](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("silent subscribe returned a valid handle"), Handle->IsValid());
		if (UTestNotifyEntity* Entity = Holder->Get())
		{
			T->TestEqual(TEXT("push applied without a delegate"), Entity->Title, FString(TEXT("silent-changed")));
		}
	}));

	TSharedPtr<bool> bUnsubscribed = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("Unsubscribe"), [bUnsubscribed, Handle]()
	{
		PlayServ::Data::Unsubscribe(*Handle);
		*bUnsubscribed = true;
	}, bUnsubscribed));

	AddSubscribeCleanupStep(this, Holder);
	return true;
}

// ---------------------------------------------------------------------------
// UnsubscribeStops — after Unsubscribe, further out-of-band changes must NOT touch the
// instance (and the handle registry is empty).
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServSubscribeUnsubscribeStopsTest,
	"PlayServ.Data.Subscribe.UnsubscribeStops",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServSubscribeUnsubscribeStopsTest::RunTest(const FString& Parameters)
{
	FSubHolder Holder = MakeShared<TStrongObjectPtr<UTestNotifyEntity>>();
	TSharedPtr<FString> Id = MakeShared<FString>();
	TSharedPtr<FPlayServSubscriptionHandle> Handle = MakeShared<FPlayServSubscriptionHandle>();

	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("subscribe-stop-test"));
	AddCreateSubjectStep(this, Holder, Id, [](UTestNotifyEntity* E)
	{
		E->Title = TEXT("stop-initial");
	});

	TSharedPtr<bool> bCycled = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("SubscribeThenUnsubscribe"), [bCycled, Holder, Handle]()
	{
		if (Holder->IsValid())
		{
			*Handle = PlayServ::Data::Subscribe(Holder->Get());
			PlayServ::Data::Unsubscribe(*Handle);
		}
		*bCycled = true;
	}, bCycled));

	AddCommand(new FPlayServAssertStep(this, [](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("handle registry empty after unsubscribe"), LiveHandleCount(), 0);
	}));

	AddOutOfBandMutateStep(this, Id, [](UTestNotifyEntity* Other)
	{
		Other->Title = TEXT("stop-changed");
	});

	// Give a would-be push ample time to arrive, then assert it did NOT apply.
	AddCommand(new FPlayServDelayStep(6.0f));

	AddCommand(new FPlayServAssertStep(this, [Holder](FAutomationTestBase* T)
	{
		if (UTestNotifyEntity* Entity = Holder->Get())
		{
			T->TestEqual(TEXT("unsubscribed instance untouched by later changes"),
				Entity->Title, FString(TEXT("stop-initial")));
		}
		else
		{
			T->AddError(TEXT("subject entity lost"));
		}
	}));

	AddSubscribeCleanupStep(this, Holder);
	return true;
}

// ---------------------------------------------------------------------------
// LocalLogoutEndsSubscriptions — Logout(Local) revokes nothing, so the platform never kills the
// dataflow socket the way a sign-out does, and the SDK has to end the subscriptions itself: after
// a local logout the handle registry is empty, and once the same player resumes, a change to the
// subscribed row no longer reaches the instance.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServSubscribeLocalLogoutEndsSubscriptionsTest,
	"PlayServ.Data.Subscribe.LocalLogoutEndsSubscriptions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServSubscribeLocalLogoutEndsSubscriptionsTest::RunTest(const FString& Parameters)
{
	FSubHolder Holder = MakeShared<TStrongObjectPtr<UTestNotifyEntity>>();
	TSharedPtr<FString> Id = MakeShared<FString>();
	TSharedPtr<FString> KeptRefresh = MakeShared<FString>();

	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("subscribe-local-logout-test"));
	AddCreateSubjectStep(this, Holder, Id, [](UTestNotifyEntity* E)
	{
		E->Title = TEXT("local-initial");
	});

	TSharedPtr<bool> bSubscribed = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("Subscribe"), [bSubscribed, Holder]()
	{
		ResetDataflowWireCounters();
		if (Holder->IsValid())
		{
			PlayServ::Data::Subscribe(Holder->Get());
		}
		*bSubscribed = true;
	}, bSubscribed));
	AddWaitForRegistrationStep(this, 1);

	TSharedPtr<bool> bLoggedOut = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("LocalLogout"), [bLoggedOut, KeptRefresh]()
	{
		UPlayServSubsystem* PS = UPlayServSubsystem::Get();
		if (PS != nullptr && PS->GetAuth() != nullptr)
		{
			*KeptRefresh = FPlayServAuthTestAccess::GetRefreshToken(PS->GetAuth());
		}
		PlayServ::Auth::Logout(FPlayServSimpleCallback::CreateLambda(
			[bLoggedOut](bool, const FPlayServError&) { *bLoggedOut = true; }), EPlayServLogoutMode::Local);
	}, bLoggedOut, 8.0f));

	AddCommand(new FPlayServAssertStep(this, [](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("local logout leaves no subscription handle"), LiveHandleCount(), 0);
	}));

	TSharedPtr<bool> bResumed = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("Resume"), [bResumed, KeptRefresh]()
	{
		PlayServ::Auth::LoginWithRefreshToken(*KeptRefresh, FPlayServAuthCallback::CreateLambda(
			[bResumed](bool, const FString&, const FPlayServError&) { *bResumed = true; }));
	}, bResumed, 8.0f));

	AddCommand(new FPlayServAssertStep(this, [](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("the same player resumed with the kept refresh token"), PlayServ::Auth::IsLoggedIn());
	}));

	AddOutOfBandMutateStep(this, Id, [](UTestNotifyEntity* Other)
	{
		Other->Title = TEXT("local-changed");
	});

	// Give a would-be push ample time to arrive, then assert it did NOT apply.
	AddCommand(new FPlayServDelayStep(6.0f));

	AddCommand(new FPlayServAssertStep(this, [Holder](FAutomationTestBase* T)
	{
		if (UTestNotifyEntity* Entity = Holder->Get())
		{
			T->TestEqual(TEXT("a subscription from before the local logout applies no later change"), Entity->Title, FString(TEXT("local-initial")));
		}
		else
		{
			T->AddError(TEXT("subject entity lost"));
		}
	}));

	AddSubscribeCleanupStep(this, Holder);
	return true;
}

// ---------------------------------------------------------------------------
// RowDeletionSendsPlatform49001 — deleting the subscribed row server-side terminates the
// subscription with a REAL platform Terminated frame carrying 49001.
//
// A synthesised termination (an inference from a record's absence) could satisfy "handles
// dropped" without the platform ever sending anything, so the assertions below read the wire
// directly: FPlayServDataflow counts the Terminated frames it
// received and records the last code, so a synthesised termination could not satisfy them.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServSubscribeRowDeletionSends49001Test,
	"PlayServ.Data.Subscribe.RowDeletionSendsPlatform49001",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServSubscribeRowDeletionSends49001Test::RunTest(const FString& Parameters)
{
	FSubHolder Holder = MakeShared<TStrongObjectPtr<UTestNotifyEntity>>();
	TSharedPtr<FString> Id = MakeShared<FString>();
	TSharedPtr<FPlayServSubscriptionHandle> Handle = MakeShared<FPlayServSubscriptionHandle>();

	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("subscribe-delete-test"));
	AddCreateSubjectStep(this, Holder, Id, [](UTestNotifyEntity* E)
	{
		E->Title = TEXT("doomed");
	});

	TSharedPtr<bool> bSubscribed = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("Subscribe"), [bSubscribed, Holder, Handle]()
	{
		// Zero the wire counters so the assertions below can only be satisfied by a frame
		// that arrives after this point.
		ResetDataflowWireCounters();
		if (Holder->IsValid())
		{
			*Handle = PlayServ::Data::Subscribe(Holder->Get());
		}
		*bSubscribed = true;
	}, bSubscribed));

	// The subscription must be LIVE on the wire before the row goes away, or the platform has
	// nothing to terminate and the test would be asserting against its own race.
	AddWaitForRegistrationStep(this, 1);

	// Delete the row out-of-band (by id — the held instance stays registered locally).
	TSharedPtr<bool> bDeleted = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("DeleteRow"), [bDeleted, Id]()
	{
		PlayServ::Data::DeleteById<UTestNotifyEntity>(*Id, FPlayServSimpleCallback::CreateLambda(
			[bDeleted](bool, const FPlayServError&) { *bDeleted = true; }));
	}, bDeleted, 10.0f));

	AddCommand(new FPlayServPollStep(this, TEXT("WaitForPlatformTermination"), []()
	{
		return FPlayServDataflow::TestTerminatedFrameCount > 0;
	}, 20.0f));

	AddCommand(new FPlayServAssertStep(this, [Holder](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("the platform sent a Terminated frame (not a client-side inference)"),
			FPlayServDataflow::TestTerminatedFrameCount > 0);
		T->TestEqual(TEXT("the platform's termination code is 49001"),
			FPlayServDataflow::TestLastTerminatedCode, 49001);
		T->TestEqual(TEXT("handles dropped after row deletion"), LiveHandleCount(), 0);
		if (UTestNotifyEntity* Entity = Holder->Get())
		{
			T->TestEqual(TEXT("instance keeps its last state"), Entity->Title, FString(TEXT("doomed")));
		}
	}));

	// Row already gone — nothing to delete, so cleanup is just releasing the handle. The session
	// stays up for the next test (PSV-2659).
	TSharedPtr<bool> bDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("ReleaseHolder"), [bDone, Holder]()
	{
		Holder->Reset();
		*bDone = true;
	}, bDone, 10.0f));
	return true;
}

// ---------------------------------------------------------------------------
// SiblingRowsAreIndependent — two records of the SAME entity type each carry their own row
// subscription: changing one notifies only that one, and deleting one terminates only that
// one while the other keeps receiving updates.
//
// Per-record independence is a property of the platform's addressing (one subscription per
// record), not of client-side routing, and that is what this pins.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServSubscribeSiblingRowsIndependentTest,
	"PlayServ.Data.Subscribe.SiblingRowsAreIndependent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServSubscribeSiblingRowsIndependentTest::RunTest(const FString& Parameters)
{
	FSubHolder HolderA = MakeShared<TStrongObjectPtr<UTestNotifyEntity>>();
	FSubHolder HolderB = MakeShared<TStrongObjectPtr<UTestNotifyEntity>>();
	TSharedPtr<FString> IdA = MakeShared<FString>();
	TSharedPtr<FString> IdB = MakeShared<FString>();
	TSharedPtr<FPlayServSubscriptionHandle> HandleA = MakeShared<FPlayServSubscriptionHandle>();
	TSharedPtr<FPlayServSubscriptionHandle> HandleB = MakeShared<FPlayServSubscriptionHandle>();
	TSharedPtr<int32> NotifiedA = MakeShared<int32>(0);
	TSharedPtr<int32> NotifiedB = MakeShared<int32>(0);

	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("subscribe-sibling-test"));
	AddCreateSubjectStep(this, HolderA, IdA, [](UTestNotifyEntity* E)
	{
		E->Title = TEXT("sibling-a");
		E->Count = 1;
	});
	AddCreateSubjectStep(this, HolderB, IdB, [](UTestNotifyEntity* E)
	{
		E->Title = TEXT("sibling-b");
		E->Count = 2;
	});

	TSharedPtr<bool> bSubscribed = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("SubscribeBoth"),
		[bSubscribed, HolderA, HolderB, HandleA, HandleB, NotifiedA, NotifiedB]()
	{
		ResetDataflowWireCounters();
		if (HolderA->IsValid())
		{
			*HandleA = PlayServ::Data::Subscribe(HolderA->Get(),
				FOnPlayServObjectChanged::CreateLambda([NotifiedA](UObject*, const TArray<FName>&)
				{
					++(*NotifiedA);
				}));
		}
		if (HolderB->IsValid())
		{
			*HandleB = PlayServ::Data::Subscribe(HolderB->Get(),
				FOnPlayServObjectChanged::CreateLambda([NotifiedB](UObject*, const TArray<FName>&)
				{
					++(*NotifiedB);
				}));
		}
		*bSubscribed = true;
	}, bSubscribed));

	AddCommand(new FPlayServAssertStep(this, [HandleA, HandleB](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("sibling A handle valid"), HandleA->IsValid());
		T->TestTrue(TEXT("sibling B handle valid"), HandleB->IsValid());
		T->TestEqual(TEXT("two independent handles registered"), LiveHandleCount(), 2);
	}));

	// Two records of one type now mean TWO row subscriptions on the wire — under the retired
	// collection model a single whole-table subscription served both.
	AddWaitForRegistrationStep(this, 2);

	AddCommand(new FPlayServAssertStep(this, [](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("the platform confirmed one row subscription per record"),
			FPlayServDataflow::TestRegisteredRowCount, 2);
	}));

	// Change ONLY A.
	AddOutOfBandMutateStep(this, IdA, [](UTestNotifyEntity* Other)
	{
		Other->Title = TEXT("sibling-a-changed");
	});

	AddCommand(new FPlayServPollStep(this, TEXT("WaitForAPush"), [NotifiedA]()
	{
		return *NotifiedA > 0;
	}, 20.0f));

	// Let any stray frame for B arrive before asserting it did not.
	AddCommand(new FPlayServDelayStep(4.0f));

	AddCommand(new FPlayServAssertStep(this, [HolderA, HolderB, NotifiedB](FAutomationTestBase* T)
	{
		if (UTestNotifyEntity* A = HolderA->Get())
		{
			T->TestEqual(TEXT("subscribed sibling A received its change"),
				A->Title, FString(TEXT("sibling-a-changed")));
		}
		else
		{
			T->AddError(TEXT("sibling A lost"));
		}
		if (UTestNotifyEntity* B = HolderB->Get())
		{
			T->TestEqual(TEXT("sibling B untouched by A's change"),
				B->Title, FString(TEXT("sibling-b")));
		}
		else
		{
			T->AddError(TEXT("sibling B lost"));
		}
		T->TestEqual(TEXT("sibling B's delegate never fired for A's change"), *NotifiedB, 0);
	}));

	// Delete A: only A's subscription must end. B stays live and still receives pushes.
	TSharedPtr<bool> bDeletedA = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("DeleteA"), [bDeletedA, IdA]()
	{
		PlayServ::Data::DeleteById<UTestNotifyEntity>(*IdA, FPlayServSimpleCallback::CreateLambda(
			[bDeletedA](bool, const FPlayServError&) { *bDeletedA = true; }));
	}, bDeletedA, 10.0f));

	AddCommand(new FPlayServPollStep(this, TEXT("WaitForAOnlyTermination"), []()
	{
		return LiveHandleCount() == 1;
	}, 20.0f));

	AddCommand(new FPlayServAssertStep(this, [](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("only sibling A's handle was dropped"), LiveHandleCount(), 1);
	}));

	// B must still be live: change it and see the push land.
	AddOutOfBandMutateStep(this, IdB, [](UTestNotifyEntity* Other)
	{
		Other->Title = TEXT("sibling-b-changed");
	});

	AddCommand(new FPlayServPollStep(this, TEXT("WaitForBPush"), [NotifiedB]()
	{
		return *NotifiedB > 0;
	}, 20.0f));

	AddCommand(new FPlayServAssertStep(this, [HolderB, NotifiedB](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("sibling B still receives pushes after A was deleted"), *NotifiedB > 0);
		if (UTestNotifyEntity* B = HolderB->Get())
		{
			T->TestEqual(TEXT("sibling B applied its own change"),
				B->Title, FString(TEXT("sibling-b-changed")));
		}
	}));

	// A's row is already deleted server-side, so only B needs tearing down.
	TSharedPtr<bool> bUnsubscribed = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("UnsubscribeB"), [bUnsubscribed, HandleB, HolderA]()
	{
		PlayServ::Data::Unsubscribe(*HandleB);
		HolderA->Reset();
		*bUnsubscribed = true;
	}, bUnsubscribed));

	AddSubscribeCleanupStep(this, HolderB);
	return true;
}

#endif // !UE_BUILD_SHIPPING
