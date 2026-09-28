#include "PlayServTestCommon.h"
#include "PlayServTestAccess.h"
#include "PlayServWireTestHelpers.h"
#include "Core/PlayServSubsystem.h"
#include "Data/PlayServData.h"
#include "Data/PlayServFilter.h"
#include "PlayServ.h"
#include "TestEntities.h"
#include "UObject/StrongObjectPtr.h"

#if !UE_BUILD_SHIPPING

// ---------------------------------------------------------------------------
// PlayServ.Data.VersionTag.*
//
// A save sends the record's version tag as If-Match, so a write made against a stale copy is
// refused (412) instead of silently overwriting a newer one. The SDK only held a tag after a
// single-record GET or a PATCH: an instance fresh from its own create, or from LoadAll, wrote
// unguarded, and an instance a realtime push had just brought up to date wrote with the tag from
// before the push and was refused although it held the newest data.
//
// The concurrent write goes over the raw wire, as another client's would: the SDK keeps one tag per
// record for the whole process, so a second SDK instance of the same record would share it.
// ---------------------------------------------------------------------------

namespace PlayServVersionTagTest
{
	using FHolder = TSharedPtr<TStrongObjectPtr<UTestNotifyEntity>>;

	struct FSaveOutcome
	{
		bool bDone = false;
		bool bSuccess = false;
		FPlayServError Error;
	};

	void AddCreateStep(FAutomationTestBase* Test, FHolder Holder, TSharedPtr<FString> Id, const FString& Title)
	{
		TSharedPtr<bool> bDone = MakeShared<bool>(false);
		Test->AddCommand(new FPlayServAsyncStep(Test, TEXT("Create"), [bDone, Holder, Id, Title]()
		{
			UTestNotifyEntity* Entity = PlayServ::Data::Create<UTestNotifyEntity>();
			Entity->Title = Title;
			Entity->Count = 1;
			*Holder = TStrongObjectPtr<UTestNotifyEntity>(Entity);
			PlayServ::Data::Save(Entity, FPlayServSimpleCallback::CreateLambda([bDone, Holder, Id](bool, const FPlayServError&)
			{
				if (Holder->IsValid())
				{
					*Id = UPlayServData::GetRecordId(Holder->Get());
				}
				*bDone = true;
			}));
		}, bDone, 8.0f));
	}

	void AddLoadStep(FAutomationTestBase* Test, FHolder Holder, TSharedPtr<FString> Id)
	{
		TSharedPtr<bool> bDone = MakeShared<bool>(false);
		Test->AddCommand(new FPlayServAsyncStep(Test, TEXT("Load"), [bDone, Holder, Id]()
		{
			PlayServ::Data::Load<UTestNotifyEntity>(*Id, [bDone, Holder](bool bOk, UTestNotifyEntity* Loaded, const FPlayServError&)
			{
				if (bOk && Loaded)
				{
					*Holder = TStrongObjectPtr<UTestNotifyEntity>(Loaded);
				}
				*bDone = true;
			});
		}, bDone, 5.0f));
	}

	void AddSaveStep(FAutomationTestBase* Test, const FString& StepName, FHolder Holder, TFunction<void(UTestNotifyEntity*)> Mutate, TSharedPtr<FSaveOutcome> Outcome)
	{
		TSharedPtr<bool> bDone = MakeShared<bool>(false);
		Test->AddCommand(new FPlayServAsyncStep(Test, StepName, [bDone, Holder, Mutate, Outcome]()
		{
			UTestNotifyEntity* Entity = Holder->Get();
			if (!Entity)
			{
				Outcome->bDone = true;
				*bDone = true;
				return;
			}
			Mutate(Entity);
			PlayServ::Data::Save(Entity, FPlayServSimpleCallback::CreateLambda([bDone, Outcome](bool bSuccess, const FPlayServError& Error)
			{
				Outcome->bSuccess = bSuccess;
				Outcome->Error = Error;
				Outcome->bDone = true;
				*bDone = true;
			}));
		}, bDone, 5.0f));
	}

	void AddDeleteStep(FAutomationTestBase* Test, TSharedPtr<FString> EntId, TSharedPtr<FString> Id)
	{
		PlayServWireTest::FWireResultPtr Result = MakeShared<PlayServWireTest::FWireResult>();
		TSharedPtr<bool> bFired = MakeShared<bool>(false);
		Test->AddCommand(new FPlayServAsyncStep(Test, TEXT("Cleanup"), [EntId, Id, Result, bFired]()
		{
			if (EntId->IsEmpty() || Id->IsEmpty())
			{
				Result->bCompleted = true;
			}
			else
			{
				PlayServWireTest::Fire(TEXT("DELETE"), FString::Printf(TEXT("/data/tables/%s/records/%s"), **EntId, **Id), TEXT("player"), nullptr, Result);
			}
			*bFired = true;
		}, bFired));
		Test->AddCommand(new FPlayServPollStep(Test, TEXT("CleanupWait"), [Result]() { return Result->bCompleted; }, 15.0f));
	}

	void AddExpectRefusedStep(FAutomationTestBase* Test, TSharedPtr<FSaveOutcome> Outcome, const FString& What)
	{
		Test->AddCommand(new FPlayServAssertStep(Test, [Outcome, What](FAutomationTestBase* T)
		{
			T->TestFalse(FString::Printf(TEXT("%s: the stale write is refused"), *What), Outcome->bSuccess);
			T->TestEqual(FString::Printf(TEXT("%s: refused as a version conflict"), *What), Outcome->Error.Code, EPlayServErrorCode::PreconditionFailed);
		}));
	}

	FString UniqueTitle(const TCHAR* Prefix)
	{
		return FString::Printf(TEXT("%s-%s"), Prefix, *FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(12));
	}

	void AddRawCreateStep(FAutomationTestBase* Test, TSharedPtr<FString> EntId, const FString& Title, TSharedPtr<FString> OutId)
	{
		PlayServWireTest::FWireResultPtr Result = MakeShared<PlayServWireTest::FWireResult>();
		TSharedPtr<bool> bFired = MakeShared<bool>(false);
		Test->AddCommand(new FPlayServAsyncStep(Test, TEXT("RawCreate"), [EntId, Title, Result, bFired]()
		{
			TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
			Body->SetStringField(TEXT("Title"), Title);
			Body->SetNumberField(TEXT("Count"), 1);
			PlayServWireTest::Fire(TEXT("POST"), FString::Printf(TEXT("/data/tables/%s/records"), **EntId), TEXT("player"), Body, Result);
			*bFired = true;
		}, bFired));
		Test->AddCommand(new FPlayServPollStep(Test, TEXT("RawCreateWait"), [Result]() { return Result->bCompleted; }, 15.0f));
		Test->AddCommand(new FPlayServAssertStep(Test, [Result, OutId](FAutomationTestBase* T)
		{
			if (T->TestEqual(TEXT("raw create answers 201"), Result->Status, 201) && Result->Json.IsValid())
			{
				Result->Json->TryGetStringField(TEXT("id"), *OutId);
			}
		}));
	}

	void AddRawPatchStep(FAutomationTestBase* Test, TSharedPtr<FString> EntId, TSharedPtr<FString> Id, int32 Count)
	{
		PlayServWireTest::FWireResultPtr Result = MakeShared<PlayServWireTest::FWireResult>();
		TSharedPtr<bool> bFired = MakeShared<bool>(false);
		Test->AddCommand(new FPlayServAsyncStep(Test, TEXT("ConcurrentWrite"), [EntId, Id, Count, Result, bFired]()
		{
			TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
			Body->SetNumberField(TEXT("Count"), Count);
			PlayServWireTest::Fire(TEXT("PATCH"), FString::Printf(TEXT("/data/tables/%s/records/%s"), **EntId, **Id), TEXT("player"), Body, Result);
			*bFired = true;
		}, bFired));
		Test->AddCommand(new FPlayServPollStep(Test, TEXT("ConcurrentWriteWait"), [Result]() { return Result->bCompleted; }, 15.0f));
		Test->AddCommand(new FPlayServAssertStep(Test, [Result](FAutomationTestBase* T)
		{
			T->TestEqual(TEXT("the concurrent write lands"), Result->Status, 200);
		}));
	}
}

// ---------------------------------------------------------------------------
// DerivedTagMatchesTheServer — the tag the SDK derives from a record's updated_at is the ETag
// the platform sends for that record (conventions §10: updated_at in ticks, hex, truncated to
// whole milliseconds). Everything below relies on that equality.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServVersionTagDerivedMatchesServerTest,
	"PlayServ.Data.VersionTag.DerivedTagMatchesTheServer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServVersionTagDerivedMatchesServerTest::RunTest(const FString& Parameters)
{
	using namespace PlayServVersionTagTest;
	FHolder Original = MakeShared<TStrongObjectPtr<UTestNotifyEntity>>();
	TSharedPtr<FString> Id = MakeShared<FString>();
	TSharedPtr<FString> EntId = MakeShared<FString>();
	PlayServWireTest::FWireResultPtr Get = MakeShared<PlayServWireTest::FWireResult>();

	AddLoginStep(this, TEXT("version-tag-derived"));
	AddCreateStep(this, Original, Id, UniqueTitle(TEXT("tag-derived")));
	PlayServWireTest::AddResolveEntityStep(this, TEXT("TestNotifyEntity"), EntId);
	TSharedPtr<bool> bFired = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("RawGet"), [EntId, Id, Get, bFired]()
	{
		PlayServWireTest::Fire(TEXT("GET"), FString::Printf(TEXT("/data/tables/%s/records/%s"), **EntId, **Id), TEXT("player"), nullptr, Get);
		*bFired = true;
	}, bFired));
	AddCommand(new FPlayServPollStep(this, TEXT("RawGetWait"), [Get]() { return Get->bCompleted; }, 15.0f));
	AddCommand(new FPlayServAssertStep(this, [Get](FAutomationTestBase* Test)
	{
		FString UpdatedAt;
		if (!Test->TestTrue(TEXT("the record has an updated_at"), Get->Json.IsValid() && Get->Json->TryGetStringField(TEXT("updated_at"), UpdatedAt)))
		{
			return;
		}
		Test->TestFalse(TEXT("the platform sent an ETag"), Get->ETag.IsEmpty());
		Test->TestEqual(TEXT("the derived tag is the platform's ETag"), FPlayServDataTestAccess::ETagForUpdatedAt(UpdatedAt), Get->ETag);
	}));
	AddDeleteStep(this, EntId, Id);
	return true;
}

// ---------------------------------------------------------------------------
// CreateGuardsTheNextWrite — an instance fresh from its own create holds the create response's
// tag, so its next write loses to a concurrent one instead of overwriting it.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServVersionTagCreateGuardsTest,
	"PlayServ.Data.VersionTag.CreateGuardsTheNextWrite",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServVersionTagCreateGuardsTest::RunTest(const FString& Parameters)
{
	using namespace PlayServVersionTagTest;
	FHolder Created = MakeShared<TStrongObjectPtr<UTestNotifyEntity>>();
	TSharedPtr<FString> Id = MakeShared<FString>();
	TSharedPtr<FString> EntId = MakeShared<FString>();
	TSharedPtr<FSaveOutcome> StaleSave = MakeShared<FSaveOutcome>();

	AddLoginStep(this, TEXT("version-tag-create"));
	PlayServWireTest::AddResolveEntityStep(this, TEXT("TestNotifyEntity"), EntId);
	AddCreateStep(this, Created, Id, UniqueTitle(TEXT("tag-create")));
	AddRawPatchStep(this, EntId, Id, 2);
	AddSaveStep(this, TEXT("StaleWrite"), Created, [](UTestNotifyEntity* E) { E->Count = 3; }, StaleSave);
	AddExpectRefusedStep(this, StaleSave, TEXT("after create"));
	AddDeleteStep(this, EntId, Id);
	return true;
}

// ---------------------------------------------------------------------------
// QueriedRowGuardsTheNextWrite — a query row carries no ETag header; its updated_at is the tag.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServVersionTagQueryGuardsTest,
	"PlayServ.Data.VersionTag.QueriedRowGuardsTheNextWrite",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServVersionTagQueryGuardsTest::RunTest(const FString& Parameters)
{
	using namespace PlayServVersionTagTest;
	FHolder Queried = MakeShared<TStrongObjectPtr<UTestNotifyEntity>>();
	TSharedPtr<FString> Id = MakeShared<FString>();
	TSharedPtr<FString> EntId = MakeShared<FString>();
	TSharedPtr<FSaveOutcome> StaleSave = MakeShared<FSaveOutcome>();
	const FString Title = UniqueTitle(TEXT("tag-query"));

	AddLoginStep(this, TEXT("version-tag-query"));
	PlayServWireTest::AddResolveEntityStep(this, TEXT("TestNotifyEntity"), EntId);
	AddRawCreateStep(this, EntId, Title, Id);
	TSharedPtr<bool> bQueried = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("LoadAll"), [bQueried, Queried, Title]()
	{
		PlayServ::Data::LoadAll<UTestNotifyEntity>(FPlayServFilter::Where(TEXT("Title")).EqualTo(Title), [bQueried, Queried](bool bOk, TArray<UTestNotifyEntity*> Rows, const FPlayServError&)
		{
			if (bOk && Rows.Num() == 1)
			{
				*Queried = TStrongObjectPtr<UTestNotifyEntity>(Rows[0]);
			}
			*bQueried = true;
		});
	}, bQueried, 5.0f));
	AddCommand(new FPlayServAssertStep(this, [Queried](FAutomationTestBase* Test)
	{
		Test->TestTrue(TEXT("the query returned the row"), Queried->IsValid());
	}));
	AddRawPatchStep(this, EntId, Id, 2);
	AddSaveStep(this, TEXT("StaleWrite"), Queried, [](UTestNotifyEntity* E) { E->Count = 3; }, StaleSave);
	AddExpectRefusedStep(this, StaleSave, TEXT("after a query"));
	AddDeleteStep(this, EntId, Id);
	return true;
}

// ---------------------------------------------------------------------------
// WriteAfterAPushLands — a realtime push brings the instance up to date, so its next write must
// not be refused with the version it held before the push. A pushed document carries no
// updated_at, so the SDK cannot learn the new version from it and sends no tag instead.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServVersionTagWriteAfterPushTest,
	"PlayServ.Data.VersionTag.WriteAfterAPushLands",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServVersionTagWriteAfterPushTest::RunTest(const FString& Parameters)
{
	using namespace PlayServVersionTagTest;
	FHolder Subscribed = MakeShared<TStrongObjectPtr<UTestNotifyEntity>>();
	TSharedPtr<FString> Id = MakeShared<FString>();
	TSharedPtr<FString> EntId = MakeShared<FString>();
	TSharedPtr<FSaveOutcome> FreshSave = MakeShared<FSaveOutcome>();
	TSharedPtr<int32> Pushes = MakeShared<int32>(0);
	TSharedPtr<FPlayServSubscriptionHandle> Handle = MakeShared<FPlayServSubscriptionHandle>();

	AddLoginStep(this, TEXT("version-tag-push"));
	PlayServWireTest::AddResolveEntityStep(this, TEXT("TestNotifyEntity"), EntId);
	AddRawCreateStep(this, EntId, UniqueTitle(TEXT("tag-push")), Id);
	AddLoadStep(this, Subscribed, Id);
	TSharedPtr<bool> bSubscribed = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("Subscribe"), [bSubscribed, Subscribed, Pushes, Handle]()
	{
		if (UTestNotifyEntity* Entity = Subscribed->Get())
		{
			*Handle = PlayServ::Data::Subscribe(Entity, FOnPlayServObjectChanged::CreateLambda([Pushes](UObject*, const TArray<FName>&) { ++*Pushes; }));
		}
		*bSubscribed = true;
	}, bSubscribed));
	AddCommand(new FPlayServDelayStep(1.5f));
	TSharedPtr<FString> TagBeforePush = MakeShared<FString>();
	AddCommand(new FPlayServAssertStep(this, [TagBeforePush, Id](FAutomationTestBase* Test)
	{
		*TagBeforePush = FPlayServDataTestAccess::VersionTagFor(UPlayServSubsystem::Get()->GetData(), *Id);
		Test->TestFalse(TEXT("the loaded instance holds a version tag"), TagBeforePush->IsEmpty());
	}));
	AddRawPatchStep(this, EntId, Id, 2);
	AddCommand(new FPlayServPollStep(this, TEXT("PushArrives"), [Pushes, Subscribed]() { return *Pushes > 0 && Subscribed->IsValid() && Subscribed->Get()->Count == 2; }, 10.0f));
	AddCommand(new FPlayServAssertStep(this, [TagBeforePush, Id](FAutomationTestBase* Test)
	{
		Test->TestNotEqual(TEXT("the tag from before the push is not the one the next save sends"), FPlayServDataTestAccess::VersionTagFor(UPlayServSubsystem::Get()->GetData(), *Id), *TagBeforePush);
	}));
	AddSaveStep(this, TEXT("FreshWrite"), Subscribed, [](UTestNotifyEntity* E) { E->Count = 5; }, FreshSave);
	AddCommand(new FPlayServAssertStep(this, [FreshSave, Handle](FAutomationTestBase* Test)
	{
		Test->TestTrue(TEXT("a write after the push lands"), FreshSave->bSuccess);
		PlayServ::Data::Unsubscribe(*Handle);
	}));
	AddDeleteStep(this, EntId, Id);
	return true;
}

#endif // !UE_BUILD_SHIPPING
