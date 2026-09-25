#include "PlayServTestCommon.h"
#include "PlayServTestAccess.h"
#include "PlayServWireTestHelpers.h"
#include "Data/PlayServData.h"
#include "PlayServ.h"
#include "TestEntities.h"
#include "Misc/Guid.h"
#include "UObject/StrongObjectPtr.h"

#if !UE_BUILD_SHIPPING

// ---------------------------------------------------------------------------
// PlayServ.Data.Singleton.*
//
// UCLASS(PlayServSingleton) is one row per project and environment. LoadSingleton reads it (the
// platform creates it with defaults on first read), Save patches the fields changed since that read under the row's
// version tag, Reload reads it again, and every call that names or deletes a row is refused.
//
// The live tests share one row on dev with every other run, so they never assume its starting values: each one
// writes the values it then checks.
// ---------------------------------------------------------------------------

static_assert(PlayServ::Data::Private::IsSingleton<UTestConfigSingleton>(), "the PlayServSingleton marking must reach the class");
static_assert(!PlayServ::Data::Private::IsSingleton<UTestPlayer>(), "an ordinary entity is not a singleton");

namespace PlayServSingletonTest
{
	using FHolder = TSharedPtr<TStrongObjectPtr<UTestConfigSingleton>>;

	struct FOutcome
	{
		bool bSuccess = false;
		FPlayServError Error;
	};

	void AddLoadStep(FAutomationTestBase* Test, const FString& StepName, FHolder Holder, TSharedPtr<FOutcome> Outcome)
	{
		TSharedPtr<bool> bDone = MakeShared<bool>(false);
		Test->AddCommand(new FPlayServAsyncStep(Test, StepName, [bDone, Holder, Outcome]()
		{
			PlayServ::Data::LoadSingleton<UTestConfigSingleton>([bDone, Holder, Outcome](bool bSuccess, UTestConfigSingleton* Config, const FPlayServError& Error)
			{
				Outcome->bSuccess = bSuccess;
				Outcome->Error = Error;
				*Holder = TStrongObjectPtr<UTestConfigSingleton>(Config);
				*bDone = true;
			});
		}, bDone, 8.0f));
	}

	void AddSaveStep(FAutomationTestBase* Test, const FString& StepName, FHolder Holder, TFunction<void(UTestConfigSingleton*)> Change, TSharedPtr<FOutcome> Outcome)
	{
		TSharedPtr<bool> bDone = MakeShared<bool>(false);
		Test->AddCommand(new FPlayServAsyncStep(Test, StepName, [Test, StepName, bDone, Holder, Change, Outcome]()
		{
			if (!Holder->IsValid())
			{
				Test->AddError(FString::Printf(TEXT("[%s] no singleton instance to save"), *StepName));
				*bDone = true;
				return;
			}
			Change(Holder->Get());
			PlayServ::Data::Save(Holder->Get(), FPlayServSimpleCallback::CreateLambda([bDone, Outcome](bool bSuccess, const FPlayServError& Error)
			{
				Outcome->bSuccess = bSuccess;
				Outcome->Error = Error;
				*bDone = true;
			}));
		}, bDone, 8.0f));
	}

	void AddReloadStep(FAutomationTestBase* Test, FHolder Holder, TSharedPtr<FOutcome> Outcome)
	{
		TSharedPtr<bool> bDone = MakeShared<bool>(false);
		Test->AddCommand(new FPlayServAsyncStep(Test, TEXT("Reload"), [bDone, Holder, Outcome]()
		{
			PlayServ::Data::Reload(Holder->Get(), FPlayServSimpleCallback::CreateLambda([bDone, Outcome](bool bSuccess, const FPlayServError& Error)
			{
				Outcome->bSuccess = bSuccess;
				Outcome->Error = Error;
				*bDone = true;
			}));
		}, bDone, 8.0f));
	}
}

// ---------------------------------------------------------------------------
// PlayServ.Data.Singleton.RefusedWithoutRead
//
// An instance LoadSingleton or Reload did not produce holds class defaults, so saving it would overwrite the shared
// row with them. Save and BulkSave refuse it before any request; Delete, BulkDelete and Subscribe refuse every
// singleton.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServSingletonRefusedWithoutReadTest,
	"PlayServ.Data.Singleton.RefusedWithoutRead",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServSingletonRefusedWithoutReadTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("is a singleton"), EAutomationExpectedErrorFlags::Contains, 0);

	UTestConfigSingleton* Fresh = NewObject<UTestConfigSingleton>();
	Fresh->Motd = TEXT("never read");
	FPlayServDataTestAccess::ResetUpsertCount();

	bool bSaveCalled = false;
	bool bSaveOk = true;
	FString SaveMessage;
	PlayServ::Data::Save(Fresh, FPlayServSimpleCallback::CreateLambda([&](bool bOk, const FPlayServError& Error)
	{
		bSaveCalled = true;
		bSaveOk = bOk;
		SaveMessage = Error.Message;
	}));
	TestTrue(TEXT("Save of an unread singleton answers at once"), bSaveCalled);
	TestFalse(TEXT("Save of an unread singleton is refused"), bSaveOk);
	TestTrue(TEXT("the refusal names LoadSingleton"), SaveMessage.Contains(TEXT("LoadSingleton")));

	bool bBulkSaveOk = true;
	PlayServ::Data::BulkSave({ Fresh }, FPlayServSimpleCallback::CreateLambda([&](bool bOk, const FPlayServError&) { bBulkSaveOk = bOk; }));
	TestFalse(TEXT("BulkSave holding an unread singleton is refused"), bBulkSaveOk);
	TestEqual(TEXT("no write left the process"), FPlayServDataTestAccess::GetUpsertCount(), 0);

	bool bDeleteOk = true;
	PlayServ::Data::Delete(Fresh, FPlayServSimpleCallback::CreateLambda([&](bool bOk, const FPlayServError&) { bDeleteOk = bOk; }));
	TestFalse(TEXT("Delete of a singleton is refused"), bDeleteOk);

	bool bBulkDeleteOk = true;
	PlayServ::Data::BulkDelete({ Fresh }, FPlayServSimpleCallback::CreateLambda([&](bool bOk, const FPlayServError&) { bBulkDeleteOk = bOk; }));
	TestFalse(TEXT("BulkDelete holding a singleton is refused"), bBulkDeleteOk);

	const FPlayServSubscriptionHandle Handle = PlayServ::Data::Subscribe(Fresh, FOnPlayServObjectChanged());
	TestFalse(TEXT("Subscribe to a singleton returns an invalid handle"), Handle.IsValid());
	PlayServ::Data::Unsubscribe(Handle);
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Data.Singleton.ServerWritesClientReads
//
// A server session writes the row and a second LoadSingleton reads the same values back; an unchanged Save sends
// nothing. A client session then reads the row (the table is open to client reads) and is refused a write (it is
// closed to client writes).
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServSingletonServerWritesClientReadsTest,
	"PlayServ.Data.Singleton.ServerWritesClientReads",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServSingletonServerWritesClientReadsTest::RunTest(const FString& Parameters)
{
	using namespace PlayServSingletonTest;

	const FString Motd = FString::Printf(TEXT("motd-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Short));
	const int32 MaxPlayers = 10 + FMath::RandRange(0, 89);

	AddCommand(new FPlayServSetupStep(this));
	PlayServWireTest::AddServerLoginStep(this);

	FHolder Written = MakeShared<TStrongObjectPtr<UTestConfigSingleton>>();
	TSharedPtr<FOutcome> LoadWritten = MakeShared<FOutcome>();
	AddLoadStep(this, TEXT("ServerLoad"), Written, LoadWritten);
	AddCommand(new FPlayServAssertStep(this, [Written, LoadWritten](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("a server session loads the singleton"), LoadWritten->bSuccess && Written->IsValid());
		if (Written->IsValid())
		{
			T->TestTrue(TEXT("a singleton has no record id"), PlayServ::Data::GetRecordId(Written->Get()).IsEmpty());
		}
	}));

	TSharedPtr<FOutcome> Saved = MakeShared<FOutcome>();
	AddSaveStep(this, TEXT("ServerSave"), Written, [Motd, MaxPlayers](UTestConfigSingleton* Config)
	{
		Config->Motd = Motd;
		Config->MaxPlayers = MaxPlayers;
	}, Saved);
	AddCommand(new FPlayServAssertStep(this, [Saved](FAutomationTestBase* T)
	{
		T->TestTrue(FString::Printf(TEXT("a server session saves the singleton (%s)"), *Saved->Error.Message), Saved->bSuccess);
	}));

	FHolder ReadBack = MakeShared<TStrongObjectPtr<UTestConfigSingleton>>();
	TSharedPtr<FOutcome> LoadReadBack = MakeShared<FOutcome>();
	AddLoadStep(this, TEXT("ServerLoadAgain"), ReadBack, LoadReadBack);
	AddCommand(new FPlayServAssertStep(this, [ReadBack, LoadReadBack, Motd, MaxPlayers](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("a second LoadSingleton succeeds"), LoadReadBack->bSuccess && ReadBack->IsValid());
		if (ReadBack->IsValid())
		{
			T->TestEqual(TEXT("the saved Motd is read back"), ReadBack->Get()->Motd, Motd);
			T->TestEqual(TEXT("the saved MaxPlayers is read back"), ReadBack->Get()->MaxPlayers, MaxPlayers);
		}
		FPlayServDataTestAccess::ResetUpsertCount();
	}));

	TSharedPtr<FOutcome> Unchanged = MakeShared<FOutcome>();
	AddSaveStep(this, TEXT("UnchangedSave"), ReadBack, [](UTestConfigSingleton*) {}, Unchanged);
	AddCommand(new FPlayServAssertStep(this, [Unchanged](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("an unchanged Save succeeds"), Unchanged->bSuccess);
		T->TestEqual(TEXT("an unchanged Save sends nothing"), FPlayServDataTestAccess::GetUpsertCount(), 0);
	}));

	AddLoginStep(this, TEXT("SingletonClient"));

	FHolder ClientCopy = MakeShared<TStrongObjectPtr<UTestConfigSingleton>>();
	TSharedPtr<FOutcome> ClientLoad = MakeShared<FOutcome>();
	AddLoadStep(this, TEXT("ClientLoad"), ClientCopy, ClientLoad);
	AddCommand(new FPlayServAssertStep(this, [ClientCopy, ClientLoad, Motd](FAutomationTestBase* T)
	{
		T->TestTrue(FString::Printf(TEXT("a client session loads the client-readable singleton (%s)"), *ClientLoad->Error.Message), ClientLoad->bSuccess && ClientCopy->IsValid());
		if (ClientCopy->IsValid())
		{
			T->TestEqual(TEXT("the client reads the value the server wrote"), ClientCopy->Get()->Motd, Motd);
		}
	}));

	TSharedPtr<FOutcome> ClientSave = MakeShared<FOutcome>();
	AddSaveStep(this, TEXT("ClientSave"), ClientCopy, [](UTestConfigSingleton* Config) { Config->Motd = TEXT("written by a client"); }, ClientSave);
	AddCommand(new FPlayServAssertStep(this, [ClientSave](FAutomationTestBase* T)
	{
		T->TestFalse(TEXT("a client write to a table closed to client writes is refused"), ClientSave->bSuccess);
		T->TestEqual(TEXT("the refusal is Forbidden"), ClientSave->Error.Code, EPlayServErrorCode::Forbidden);
	}));
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Data.Singleton.StaleWriteIsRefused
//
// Another writer changes the row after this process read it: Save is refused PreconditionFailed rather than
// overwriting, Reload brings the other write in, and the Save after it goes through. The other writer is the raw
// wire, as another server would be; a second SDK instance in this process would share the version tag.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServSingletonStaleWriteIsRefusedTest,
	"PlayServ.Data.Singleton.StaleWriteIsRefused",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServSingletonStaleWriteIsRefusedTest::RunTest(const FString& Parameters)
{
	using namespace PlayServSingletonTest;

	const FString OtherMotd = FString::Printf(TEXT("other-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Short));
	const int32 MaxPlayers = 100 + FMath::RandRange(0, 899);

	AddCommand(new FPlayServSetupStep(this));
	PlayServWireTest::AddServerLoginStep(this);
	TSharedPtr<FString> EntId = MakeShared<FString>();
	PlayServWireTest::AddResolveEntityStep(this, TEXT("TestConfigSingleton"), EntId);

	FHolder Config = MakeShared<TStrongObjectPtr<UTestConfigSingleton>>();
	TSharedPtr<FOutcome> Loaded = MakeShared<FOutcome>();
	AddLoadStep(this, TEXT("Load"), Config, Loaded);

	PlayServWireTest::FWireResultPtr OtherWrite = MakeShared<PlayServWireTest::FWireResult>();
	TSharedPtr<bool> bOtherFired = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("OtherWriter"), [EntId, OtherMotd, OtherWrite, bOtherFired]()
	{
		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("Motd"), OtherMotd);
		PlayServWireTest::Fire(TEXT("PATCH"), FString::Printf(TEXT("/data/tables/%s/singleton"), **EntId), TEXT("server"), Body, OtherWrite);
		*bOtherFired = true;
	}, bOtherFired));
	AddCommand(new FPlayServPollStep(this, TEXT("OtherWriterWait"), [OtherWrite]() { return OtherWrite->bCompleted; }, 15.0f));
	AddCommand(new FPlayServAssertStep(this, [Loaded, OtherWrite](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("the singleton loaded"), Loaded->bSuccess);
		T->TestEqual(TEXT("the other writer's PATCH landed"), OtherWrite->Status, 200);
	}));

	TSharedPtr<FOutcome> Stale = MakeShared<FOutcome>();
	AddSaveStep(this, TEXT("StaleSave"), Config, [MaxPlayers](UTestConfigSingleton* Instance) { Instance->MaxPlayers = MaxPlayers; }, Stale);
	AddCommand(new FPlayServAssertStep(this, [Stale](FAutomationTestBase* T)
	{
		T->TestFalse(TEXT("a Save from before the other write is refused"), Stale->bSuccess);
		T->TestEqual(TEXT("the refusal is PreconditionFailed"), Stale->Error.Code, EPlayServErrorCode::PreconditionFailed);
	}));

	TSharedPtr<FOutcome> Reloaded = MakeShared<FOutcome>();
	AddReloadStep(this, Config, Reloaded);
	AddCommand(new FPlayServAssertStep(this, [Config, Reloaded, OtherMotd](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("Reload succeeds"), Reloaded->bSuccess);
		if (Config->IsValid())
		{
			T->TestEqual(TEXT("Reload brings the other write in"), Config->Get()->Motd, OtherMotd);
		}
	}));

	TSharedPtr<FOutcome> Retried = MakeShared<FOutcome>();
	AddSaveStep(this, TEXT("RetriedSave"), Config, [MaxPlayers](UTestConfigSingleton* Instance) { Instance->MaxPlayers = MaxPlayers; }, Retried);

	FHolder Check = MakeShared<TStrongObjectPtr<UTestConfigSingleton>>();
	TSharedPtr<FOutcome> CheckLoad = MakeShared<FOutcome>();
	AddLoadStep(this, TEXT("CheckLoad"), Check, CheckLoad);
	AddCommand(new FPlayServAssertStep(this, [Retried, Check, OtherMotd, MaxPlayers](FAutomationTestBase* T)
	{
		T->TestTrue(FString::Printf(TEXT("the Save after Reload goes through (%s)"), *Retried->Error.Message), Retried->bSuccess);
		if (Check->IsValid())
		{
			T->TestEqual(TEXT("the retried change is on the row"), Check->Get()->MaxPlayers, MaxPlayers);
			T->TestEqual(TEXT("the other writer's change survived it"), Check->Get()->Motd, OtherMotd);
		}
	}));
	return true;
}

#endif // !UE_BUILD_SHIPPING
