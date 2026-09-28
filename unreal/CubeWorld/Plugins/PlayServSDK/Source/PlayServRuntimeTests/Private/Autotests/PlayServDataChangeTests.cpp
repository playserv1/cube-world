#include "PlayServTestCommon.h"
#include "PlayServTestAccess.h"
#include "PlayServWireTestHelpers.h"
#include "Core/PlayServSubsystem.h"
#include "Data/PlayServData.h"
#include "PlayServ.h"
#include "Realtime/PlayServDataflow.h"
#include "TestEntities.h"
#include "UObject/StrongObjectPtr.h"

#if !UE_BUILD_SHIPPING

// ---------------------------------------------------------------------------
// What an overwrite from the platform reports, and to whom.
//
// A push, a Reload, or a Populate of an instance that already holds backend data overwrites the instance and then
// reports the top-level fields whose value is different afterwards: first to the fields' PlayServOnChanged
// functions, in declaration order, then to the Subscribe delegates bound to that instance. "Different" is judged
// on the values as the platform stores them, the same test Save uses, so a reference to the same record is not a
// change. Load, Save and local writes report nothing.
//
// Live on dev with a client session, except PartialEntityKeepsLocalFields (a server session: the table is closed
// to client writes).
// ---------------------------------------------------------------------------

namespace PlayServChangeTest
{
	using FNotifyHolder = TSharedPtr<TStrongObjectPtr<UTestNotifyEntity>>;

	struct FDone
	{
		bool bDone = false;
		bool bSuccess = false;
		FPlayServError Error;
	};

	void AddCreateNotifyStep(FAutomationTestBase* Test, FNotifyHolder Holder, TSharedPtr<FString> Id, TFunction<void(UTestNotifyEntity*)> Init)
	{
		TSharedPtr<bool> bDone = MakeShared<bool>(false);
		Test->AddCommand(new FPlayServAsyncStep(Test, TEXT("CreateSubject"), [Test, bDone, Holder, Id, Init]()
		{
			UTestNotifyEntity* Entity = PlayServ::Data::Create<UTestNotifyEntity>();
			Init(Entity);
			*Holder = TStrongObjectPtr<UTestNotifyEntity>(Entity);
			PlayServ::Data::Save(Entity, FPlayServSimpleCallback::CreateLambda([Test, bDone, Id, Holder](bool bOk, const FPlayServError& Error)
			{
				if (!bOk)
				{
					Test->AddError(FString::Printf(TEXT("creating the subject failed: %s"), *Error.Message));
				}
				if (Holder->IsValid())
				{
					*Id = UPlayServData::GetRecordId(Holder->Get());
				}
				*bDone = true;
			}));
		}, bDone, 10.0f));
	}

	template<typename T>
	void AddOutOfBandMutateStep(FAutomationTestBase* Test, TSharedPtr<FString> Id, TFunction<void(T*)> Mutator)
	{
		TSharedPtr<bool> bDone = MakeShared<bool>(false);
		Test->AddCommand(new FPlayServAsyncStep(Test, TEXT("OutOfBandMutate"), [Test, bDone, Id, Mutator]()
		{
			PlayServ::Data::Load<T>(*Id, [Test, bDone, Mutator](bool bLoad, T* Other, const FPlayServError& Error)
			{
				if (!bLoad || !Other)
				{
					Test->AddError(FString::Printf(TEXT("loading the second instance failed: %s"), *Error.Message));
					*bDone = true;
					return;
				}
				Mutator(Other);
				PlayServ::Data::Save(Other, FPlayServSimpleCallback::CreateLambda([bDone](bool, const FPlayServError&) { *bDone = true; }));
			});
		}, bDone, 10.0f));
	}

	void AddWaitForRegistrationStep(FAutomationTestBase* Test, int32 ExpectedRows)
	{
		Test->AddCommand(new FPlayServPollStep(Test, TEXT("WaitForRegistration"), [ExpectedRows]()
		{
			return FPlayServDataflow::TestRegisteredRowCount >= ExpectedRows;
		}, 20.0f));
		Test->AddCommand(new FPlayServDelayStep(1.0f));
	}

	void AddReloadStep(FAutomationTestBase* Test, TFunction<UObject*()> EntityOf, TSharedPtr<FDone> Outcome)
	{
		TSharedPtr<bool> bDone = MakeShared<bool>(false);
		Test->AddCommand(new FPlayServAsyncStep(Test, TEXT("Reload"), [bDone, EntityOf, Outcome]()
		{
			PlayServ::Data::Reload(EntityOf(), FPlayServSimpleCallback::CreateLambda([bDone, Outcome](bool bOk, const FPlayServError& Error)
			{
				Outcome->bSuccess = bOk;
				Outcome->Error = Error;
				Outcome->bDone = true;
				*bDone = true;
			}));
		}, bDone, 10.0f));
	}

	void AddDeleteStep(FAutomationTestBase* Test, TFunction<UObject*()> EntityOf)
	{
		TSharedPtr<bool> bDone = MakeShared<bool>(false);
		Test->AddCommand(new FPlayServAsyncStep(Test, TEXT("Cleanup"), [bDone, EntityOf]()
		{
			UObject* Entity = EntityOf();
			if (Entity == nullptr || UPlayServData::GetRecordId(Entity).IsEmpty())
			{
				*bDone = true;
				return;
			}
			PlayServ::Data::Delete(Entity, FPlayServSimpleCallback::CreateLambda([bDone](bool, const FPlayServError&) { *bDone = true; }));
		}, bDone, 10.0f));
	}
}

// ---------------------------------------------------------------------------
// PlayServ.Data.Subscribe.UnchangedReferenceIsNotReported
//
// A push that changes only Name reports Name. The Clan reference points at the same record before and after, but
// each apply builds a new stub for it, so a comparison of the objects called it changed on every push.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServSubscribeUnchangedReferenceIsNotReportedTest,
	"PlayServ.Data.Subscribe.UnchangedReferenceIsNotReported",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServSubscribeUnchangedReferenceIsNotReportedTest::RunTest(const FString& Parameters)
{
	using namespace PlayServChangeTest;

	TSharedPtr<TStrongObjectPtr<UTestClan>> Clan = MakeShared<TStrongObjectPtr<UTestClan>>();
	TSharedPtr<TStrongObjectPtr<UTestPlayer>> Player = MakeShared<TStrongObjectPtr<UTestPlayer>>();
	TSharedPtr<FString> PlayerId = MakeShared<FString>();
	TSharedPtr<FPlayServSubscriptionHandle> Handle = MakeShared<FPlayServSubscriptionHandle>();
	TSharedPtr<bool> bNotified = MakeShared<bool>(false);
	TSharedPtr<TArray<FName>> Changed = MakeShared<TArray<FName>>();

	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("Subscribe.UnchangedReferenceIsNotReported"));

	TSharedPtr<bool> bCreated = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("CreateClanAndPlayer"), [this, bCreated, Clan, Player, PlayerId]()
	{
		UTestClan* NewClan = PlayServ::Data::Create<UTestClan>();
		NewClan->ClanName = TEXT("ref-diff clan");
		*Clan = TStrongObjectPtr<UTestClan>(NewClan);
		PlayServ::Data::Save(NewClan, FPlayServSimpleCallback::CreateLambda([this, bCreated, Clan, Player, PlayerId](bool bClanOk, const FPlayServError& ClanError)
		{
			if (!bClanOk)
			{
				AddError(FString::Printf(TEXT("creating the clan failed: %s"), *ClanError.Message));
				*bCreated = true;
				return;
			}
			UTestPlayer* NewPlayer = PlayServ::Data::Create<UTestPlayer>();
			NewPlayer->Name = TEXT("before");
			NewPlayer->Clan = Clan->Get();
			*Player = TStrongObjectPtr<UTestPlayer>(NewPlayer);
			PlayServ::Data::Save(NewPlayer, FPlayServSimpleCallback::CreateLambda([this, bCreated, Player, PlayerId](bool bOk, const FPlayServError& Error)
			{
				if (!bOk)
				{
					AddError(FString::Printf(TEXT("creating the player failed: %s"), *Error.Message));
				}
				*PlayerId = UPlayServData::GetRecordId(Player->Get());
				*bCreated = true;
			}));
		}));
	}, bCreated, 10.0f));

	TSharedPtr<bool> bSubscribed = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("Subscribe"), [bSubscribed, Player, Handle, bNotified, Changed]()
	{
		FPlayServDataflow::TestRegisteredRowCount = 0;
		*Handle = PlayServ::Data::Subscribe(Player->Get(), FOnPlayServObjectChanged::CreateLambda([bNotified, Changed](UObject*, const TArray<FName>& InChanged)
		{
			*Changed = InChanged;
			*bNotified = true;
		}));
		*bSubscribed = true;
	}, bSubscribed));
	AddWaitForRegistrationStep(this, 1);
	AddCommand(new FPlayServAssertStep(this, [bNotified](FAutomationTestBase*) { *bNotified = false; }));

	AddOutOfBandMutateStep<UTestPlayer>(this, PlayerId, [](UTestPlayer* Other) { Other->Name = TEXT("after"); });
	AddCommand(new FPlayServPollStep(this, TEXT("WaitForPush"), [bNotified]() { return *bNotified; }, 20.0f));
	AddCommand(new FPlayServAssertStep(this, [Changed](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("the changed field is reported"), Changed->Contains(FName(TEXT("Name"))));
		T->TestFalse(TEXT("a reference to the same record is not reported"), Changed->Contains(FName(TEXT("Clan"))));
		T->TestEqual(TEXT("only the changed field is reported"), Changed->Num(), 1);
	}));

	AddCommand(new FPlayServAssertStep(this, [Handle](FAutomationTestBase*) { PlayServ::Data::Unsubscribe(*Handle); }));
	AddDeleteStep(this, [Player]() -> UObject* { return Player->Get(); });
	AddDeleteStep(this, [Clan]() -> UObject* { return Clan->Get(); });
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Data.Subscribe.DelegateMayChangeSubscriptions
//
// Two handles on one instance; the first delegate subscribes 64 more handles and ends the second one. The dispatch
// held pointers into the handle map across user code, so growing the map left them dangling. Now: the first
// delegate runs once and the ended handle's delegate does not run.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServSubscribeDelegateMayChangeSubscriptionsTest,
	"PlayServ.Data.Subscribe.DelegateMayChangeSubscriptions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServSubscribeDelegateMayChangeSubscriptionsTest::RunTest(const FString& Parameters)
{
	using namespace PlayServChangeTest;

	FNotifyHolder Holder = MakeShared<TStrongObjectPtr<UTestNotifyEntity>>();
	TSharedPtr<FString> Id = MakeShared<FString>();
	TSharedPtr<FPlayServSubscriptionHandle> First = MakeShared<FPlayServSubscriptionHandle>();
	TSharedPtr<FPlayServSubscriptionHandle> Second = MakeShared<FPlayServSubscriptionHandle>();
	TSharedPtr<TArray<FPlayServSubscriptionHandle>> Extra = MakeShared<TArray<FPlayServSubscriptionHandle>>();
	TSharedPtr<int32> FirstCalls = MakeShared<int32>(0);
	TSharedPtr<int32> SecondCalls = MakeShared<int32>(0);
	TSharedPtr<bool> bArmed = MakeShared<bool>(false);

	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("Subscribe.DelegateMayChangeSubscriptions"));
	AddCreateNotifyStep(this, Holder, Id, [](UTestNotifyEntity* Entity) { Entity->Title = TEXT("start"); });

	TSharedPtr<bool> bSubscribed = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("SubscribeTwice"), [bSubscribed, Holder, First, Second, Extra, FirstCalls, SecondCalls, bArmed]()
	{
		FPlayServDataflow::TestRegisteredRowCount = 0;
		*First = PlayServ::Data::Subscribe(Holder->Get(), FOnPlayServObjectChanged::CreateLambda([Holder, Second, Extra, FirstCalls, bArmed](UObject*, const TArray<FName>&)
		{
			if (!*bArmed)
			{
				return;
			}
			++*FirstCalls;
			for (int32 Index = 0; Index < 64; ++Index)
			{
				Extra->Add(PlayServ::Data::Subscribe(Holder->Get()));
			}
			PlayServ::Data::Unsubscribe(*Second);
		}));
		*Second = PlayServ::Data::Subscribe(Holder->Get(), FOnPlayServObjectChanged::CreateLambda([SecondCalls, bArmed](UObject*, const TArray<FName>&)
		{
			if (*bArmed)
			{
				++*SecondCalls;
			}
		}));
		*bSubscribed = true;
	}, bSubscribed));
	AddWaitForRegistrationStep(this, 1);
	AddCommand(new FPlayServAssertStep(this, [bArmed](FAutomationTestBase*) { *bArmed = true; }));

	AddOutOfBandMutateStep<UTestNotifyEntity>(this, Id, [](UTestNotifyEntity* Other) { Other->Title = TEXT("pushed"); });
	AddCommand(new FPlayServPollStep(this, TEXT("WaitForPush"), [FirstCalls]() { return *FirstCalls > 0; }, 20.0f));
	AddCommand(new FPlayServDelayStep(0.5f));
	AddCommand(new FPlayServAssertStep(this, [FirstCalls, SecondCalls](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("the first delegate ran once"), *FirstCalls, 1);
		T->TestEqual(TEXT("the handle it ended did not run"), *SecondCalls, 0);
	}));

	AddCommand(new FPlayServAssertStep(this, [First, Extra](FAutomationTestBase*)
	{
		PlayServ::Data::Unsubscribe(*First);
		for (const FPlayServSubscriptionHandle& Handle : *Extra)
		{
			PlayServ::Data::Unsubscribe(Handle);
		}
	}));
	AddDeleteStep(this, [Holder]() -> UObject* { return Holder->Get(); });
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Data.Reload.PartialEntityKeepsLocalFields
//
// A partial entity persists only its PlayServProperty fields; the others are the game's. Reload reset every
// property to the class default before applying the document, so the unmarked ones were lost.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServReloadPartialEntityKeepsLocalFieldsTest,
	"PlayServ.Data.Reload.PartialEntityKeepsLocalFields",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServReloadPartialEntityKeepsLocalFieldsTest::RunTest(const FString& Parameters)
{
	using namespace PlayServChangeTest;

	TSharedPtr<TStrongObjectPtr<UTestPartialEntity>> Holder = MakeShared<TStrongObjectPtr<UTestPartialEntity>>();

	AddCommand(new FPlayServSetupStep(this));
	PlayServWireTest::AddServerLoginStep(this);

	TSharedPtr<bool> bSaved = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("Create"), [this, bSaved, Holder]()
	{
		UTestPartialEntity* Entity = PlayServ::Data::Create<UTestPartialEntity>();
		Entity->PersistedName = TEXT("partial");
		*Holder = TStrongObjectPtr<UTestPartialEntity>(Entity);
		PlayServ::Data::Save(Entity, FPlayServSimpleCallback::CreateLambda([this, bSaved](bool bOk, const FPlayServError& Error)
		{
			if (!bOk)
			{
				AddError(FString::Printf(TEXT("creating the partial entity failed: %s"), *Error.Message));
			}
			*bSaved = true;
		}));
	}, bSaved, 10.0f));

	AddCommand(new FPlayServAssertStep(this, [Holder](FAutomationTestBase*)
	{
		Holder->Get()->LocalNote = TEXT("keep");
		Holder->Get()->LocalCounter = 7;
	}));

	TSharedPtr<FDone> Reloaded = MakeShared<FDone>();
	AddReloadStep(this, [Holder]() -> UObject* { return Holder->Get(); }, Reloaded);
	AddCommand(new FPlayServAssertStep(this, [Holder, Reloaded](FAutomationTestBase* T)
	{
		T->TestTrue(FString::Printf(TEXT("Reload succeeds (%s)"), *Reloaded->Error.Message), Reloaded->bSuccess);
		T->TestEqual(TEXT("an unmarked field survives Reload"), Holder->Get()->LocalNote, FString(TEXT("keep")));
		T->TestEqual(TEXT("another unmarked field survives Reload"), Holder->Get()->LocalCounter, 7);
		T->TestEqual(TEXT("the persisted field is the platform's"), Holder->Get()->PersistedName, FString(TEXT("partial")));
	}));
	AddDeleteStep(this, [Holder]() -> UObject* { return Holder->Get(); });
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Data.Reload.NotifiesSubscribers
//
// Reload overwrites an unsaved local edit; the instance's Subscribe delegate hears about it with the field names,
// as it would from a push, and before the Reload callback.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServReloadNotifiesSubscribersTest,
	"PlayServ.Data.Reload.NotifiesSubscribers",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServReloadNotifiesSubscribersTest::RunTest(const FString& Parameters)
{
	using namespace PlayServChangeTest;

	FNotifyHolder Holder = MakeShared<TStrongObjectPtr<UTestNotifyEntity>>();
	TSharedPtr<FString> Id = MakeShared<FString>();
	TSharedPtr<FPlayServSubscriptionHandle> Handle = MakeShared<FPlayServSubscriptionHandle>();
	TSharedPtr<TArray<FName>> Changed = MakeShared<TArray<FName>>();
	TSharedPtr<int32> Notifications = MakeShared<int32>(0);

	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("Reload.NotifiesSubscribers"));
	AddCreateNotifyStep(this, Holder, Id, [](UTestNotifyEntity* Entity)
	{
		Entity->Title = TEXT("saved");
		Entity->Count = 3;
	});

	TSharedPtr<bool> bSubscribed = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("Subscribe"), [bSubscribed, Holder, Handle, Changed, Notifications]()
	{
		FPlayServDataflow::TestRegisteredRowCount = 0;
		*Handle = PlayServ::Data::Subscribe(Holder->Get(), FOnPlayServObjectChanged::CreateLambda([Holder, Changed, Notifications](UObject*, const TArray<FName>& InChanged)
		{
			*Changed = InChanged;
			++*Notifications;
			Holder->Get()->Calls.Add(TEXT("Delegate"));
		}));
		*bSubscribed = true;
	}, bSubscribed));
	AddWaitForRegistrationStep(this, 1);

	AddCommand(new FPlayServAssertStep(this, [Holder, Notifications](FAutomationTestBase*)
	{
		*Notifications = 0;
		Holder->Get()->Calls.Reset();
		Holder->Get()->Title = TEXT("unsaved edit");
	}));

	TSharedPtr<FDone> Reloaded = MakeShared<FDone>();
	TSharedPtr<int32> NotificationsAtCallback = MakeShared<int32>(-1);
	TSharedPtr<bool> bReloadDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("Reload"), [bReloadDone, Holder, Reloaded, Notifications, NotificationsAtCallback]()
	{
		PlayServ::Data::Reload(Holder->Get(), FPlayServSimpleCallback::CreateLambda([bReloadDone, Reloaded, Notifications, NotificationsAtCallback](bool bOk, const FPlayServError& Error)
		{
			*NotificationsAtCallback = *Notifications;
			Reloaded->bSuccess = bOk;
			Reloaded->Error = Error;
			*bReloadDone = true;
		}));
	}, bReloadDone, 10.0f));

	AddCommand(new FPlayServAssertStep(this, [Holder, Reloaded, Changed, Notifications, NotificationsAtCallback](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("Reload succeeds"), Reloaded->bSuccess);
		T->TestEqual(TEXT("Reload restored the platform's value"), Holder->Get()->Title, FString(TEXT("saved")));
		T->TestEqual(TEXT("the subscriber heard once"), *Notifications, 1);
		T->TestEqual(TEXT("before the Reload callback"), *NotificationsAtCallback, 1);
		T->TestTrue(TEXT("with the overwritten field"), Changed->Contains(FName(TEXT("Title"))));
		T->TestFalse(TEXT("and not an unchanged one"), Changed->Contains(FName(TEXT("Count"))));
	}));

	AddCommand(new FPlayServAssertStep(this, [Handle](FAutomationTestBase*) { PlayServ::Data::Unsubscribe(*Handle); }));
	AddDeleteStep(this, [Holder]() -> UObject* { return Holder->Get(); });
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Data.ChangeHooks.ReloadCallsTheChangedFieldsFunctions
//
// Reload overwrites two unsaved local edits: each edited field's PlayServOnChanged function runs once, in
// declaration order, with the value the instance held before; the unchanged field's does not.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServChangeHooksReloadCallsTheChangedFieldsFunctionsTest,
	"PlayServ.Data.ChangeHooks.ReloadCallsTheChangedFieldsFunctions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServChangeHooksReloadCallsTheChangedFieldsFunctionsTest::RunTest(const FString& Parameters)
{
	using namespace PlayServChangeTest;

	FNotifyHolder Holder = MakeShared<TStrongObjectPtr<UTestNotifyEntity>>();
	TSharedPtr<FString> Id = MakeShared<FString>();

	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("ChangeHooks.ReloadCallsTheChangedFieldsFunctions"));
	AddCreateNotifyStep(this, Holder, Id, [](UTestNotifyEntity* Entity)
	{
		Entity->Title = TEXT("saved");
		Entity->Count = 1;
		Entity->Profile.Bio = TEXT("bio");
	});

	AddCommand(new FPlayServAssertStep(this, [Holder](FAutomationTestBase*)
	{
		Holder->Get()->Calls.Reset();
		Holder->Get()->Title = TEXT("edited");
		Holder->Get()->Count = 9;
	}));

	TSharedPtr<FDone> Reloaded = MakeShared<FDone>();
	AddReloadStep(this, [Holder]() -> UObject* { return Holder->Get(); }, Reloaded);
	AddCommand(new FPlayServAssertStep(this, [Holder, Reloaded](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("Reload succeeds"), Reloaded->bSuccess);
		const TArray<FString>& Calls = Holder->Get()->Calls;
		T->TestEqual(TEXT("two functions ran"), Calls.Num(), 2);
		if (Calls.Num() == 2)
		{
			T->TestEqual(TEXT("Title's first, with the value it held"), Calls[0], FString(TEXT("Title:edited")));
			T->TestEqual(TEXT("then Count's"), Calls[1], FString(TEXT("Count")));
		}
	}));
	AddDeleteStep(this, [Holder]() -> UObject* { return Holder->Get(); });
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Data.ChangeHooks.NoCallWithoutAnOverwrite
//
// Load builds a new instance and Save writes from one; neither overwrites a value the instance held, so no
// PlayServOnChanged function runs. Nor does a Reload that finds every value unchanged.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServChangeHooksNoCallWithoutAnOverwriteTest,
	"PlayServ.Data.ChangeHooks.NoCallWithoutAnOverwrite",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServChangeHooksNoCallWithoutAnOverwriteTest::RunTest(const FString& Parameters)
{
	using namespace PlayServChangeTest;

	FNotifyHolder Holder = MakeShared<TStrongObjectPtr<UTestNotifyEntity>>();
	FNotifyHolder Loaded = MakeShared<TStrongObjectPtr<UTestNotifyEntity>>();
	TSharedPtr<FString> Id = MakeShared<FString>();

	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("ChangeHooks.NoCallWithoutAnOverwrite"));
	AddCreateNotifyStep(this, Holder, Id, [](UTestNotifyEntity* Entity) { Entity->Title = TEXT("first"); });

	TSharedPtr<bool> bLoaded = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("Load"), [bLoaded, Id, Loaded]()
	{
		PlayServ::Data::Load<UTestNotifyEntity>(*Id, [bLoaded, Loaded](bool, UTestNotifyEntity* Entity, const FPlayServError&)
		{
			*Loaded = TStrongObjectPtr<UTestNotifyEntity>(Entity);
			*bLoaded = true;
		});
	}, bLoaded, 10.0f));

	TSharedPtr<bool> bSaved = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("Save"), [bSaved, Loaded]()
	{
		Loaded->Get()->Title = TEXT("second");
		PlayServ::Data::Save(Loaded->Get(), FPlayServSimpleCallback::CreateLambda([bSaved](bool, const FPlayServError&) { *bSaved = true; }));
	}, bSaved, 10.0f));

	TSharedPtr<FDone> Reloaded = MakeShared<FDone>();
	AddReloadStep(this, [Loaded]() -> UObject* { return Loaded->Get(); }, Reloaded);
	AddCommand(new FPlayServAssertStep(this, [Holder, Loaded, Reloaded](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("the unchanged Reload succeeds"), Reloaded->bSuccess);
		T->TestEqual(TEXT("no function ran on the created and saved instance"), Holder->Get()->Calls.Num(), 0);
		T->TestEqual(TEXT("none on the loaded, saved and reloaded one"), Loaded->Get()->Calls.Num(), 0);
	}));
	AddDeleteStep(this, [Loaded]() -> UObject* { return Loaded->Get(); });
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Data.ChangeHooks.PushCallsFunctionsBeforeTheDelegate
//
// A push that changes Title and Profile runs Title's and Profile's functions, in declaration order and with the
// values the instance held, and only then the Subscribe delegate.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServChangeHooksPushCallsFunctionsBeforeTheDelegateTest,
	"PlayServ.Data.ChangeHooks.PushCallsFunctionsBeforeTheDelegate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServChangeHooksPushCallsFunctionsBeforeTheDelegateTest::RunTest(const FString& Parameters)
{
	using namespace PlayServChangeTest;

	FNotifyHolder Holder = MakeShared<TStrongObjectPtr<UTestNotifyEntity>>();
	TSharedPtr<FString> Id = MakeShared<FString>();
	TSharedPtr<FPlayServSubscriptionHandle> Handle = MakeShared<FPlayServSubscriptionHandle>();
	TSharedPtr<bool> bNotified = MakeShared<bool>(false);

	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("ChangeHooks.PushCallsFunctionsBeforeTheDelegate"));
	AddCreateNotifyStep(this, Holder, Id, [](UTestNotifyEntity* Entity)
	{
		Entity->Title = TEXT("old title");
		Entity->Profile.Bio = TEXT("old bio");
	});

	TSharedPtr<bool> bSubscribed = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("Subscribe"), [bSubscribed, Holder, Handle, bNotified]()
	{
		FPlayServDataflow::TestRegisteredRowCount = 0;
		*Handle = PlayServ::Data::Subscribe(Holder->Get(), FOnPlayServObjectChanged::CreateLambda([Holder, bNotified](UObject*, const TArray<FName>&)
		{
			Holder->Get()->Calls.Add(TEXT("Delegate"));
			*bNotified = true;
		}));
		*bSubscribed = true;
	}, bSubscribed));
	AddWaitForRegistrationStep(this, 1);
	AddCommand(new FPlayServAssertStep(this, [Holder, bNotified](FAutomationTestBase*)
	{
		Holder->Get()->Calls.Reset();
		*bNotified = false;
	}));

	AddOutOfBandMutateStep<UTestNotifyEntity>(this, Id, [](UTestNotifyEntity* Other)
	{
		Other->Title = TEXT("new title");
		Other->Profile.Bio = TEXT("new bio");
	});
	AddCommand(new FPlayServPollStep(this, TEXT("WaitForPush"), [bNotified]() { return *bNotified; }, 20.0f));
	AddCommand(new FPlayServAssertStep(this, [Holder](FAutomationTestBase* T)
	{
		const TArray<FString>& Calls = Holder->Get()->Calls;
		T->TestEqual(TEXT("two functions and the delegate ran"), Calls.Num(), 3);
		if (Calls.Num() == 3)
		{
			T->TestEqual(TEXT("Title's function first, with the old title"), Calls[0], FString(TEXT("Title:old title")));
			T->TestEqual(TEXT("Profile's next, with the old profile"), Calls[1], FString(TEXT("Profile:old bio")));
			T->TestEqual(TEXT("the delegate last"), Calls[2], FString(TEXT("Delegate")));
		}
	}));

	AddCommand(new FPlayServAssertStep(this, [Handle](FAutomationTestBase*) { PlayServ::Data::Unsubscribe(*Handle); }));
	AddDeleteStep(this, [Holder]() -> UObject* { return Holder->Get(); });
	return true;
}

#endif // !UE_BUILD_SHIPPING
