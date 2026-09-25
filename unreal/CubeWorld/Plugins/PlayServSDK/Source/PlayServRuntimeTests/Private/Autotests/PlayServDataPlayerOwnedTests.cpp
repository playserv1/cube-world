#include "PlayServTestCommon.h"
#include "PlayServTestAccess.h"
#include "PlayServWireTestHelpers.h"
#include "Data/PlayServData.h"
#include "GameFramework/PlayerController.h"
#include "PlayServ.h"
#include "TestEntities.h"
#include "Misc/Guid.h"
#include "UObject/StrongObjectPtr.h"

#if !UE_BUILD_SHIPPING

// ---------------------------------------------------------------------------
// PlayServ.Data.PlayerOwned.*
//
// A UCLASS(PlayServEntity, PlayServPlayerOwned) class holds one row per player in a player-owned table; the table
// keeps the row's player in a player_id field the class does not declare. LoadPlayerOwned finds the row by it. A
// client creates its own missing row where the table is open to client writes; a server loads and saves rows but
// never creates one.
//
// Tests that need a row to be created mint a fresh player, so "created" is certain.
// ---------------------------------------------------------------------------

static_assert(PlayServ::Data::Private::IsPlayerOwned<UTestPlayerProfile>(), "the PlayServPlayerOwned marking must reach the class");
static_assert(!PlayServ::Data::Private::IsPlayerOwned<UTestPlayer>(), "an ordinary entity is not player-owned");

namespace PlayServPlayerOwnedTest
{
	struct FRowOutcome
	{
		bool bDone = false;
		bool bSuccess = false;
		bool bCreated = false;
		FString RecordId;
		FPlayServError Error;
	};

	template<typename T>
	using THolder = TSharedPtr<TStrongObjectPtr<T>>;

	FString SyntheticPlayerId()
	{
		return FString::Printf(TEXT("plr_UESDKTEST%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	}

	void AddFreshPlayerStep(FAutomationTestBase* Test, const FString& Label, TSharedPtr<FString> OutPlayerId)
	{
		TSharedPtr<bool> bDone = MakeShared<bool>(false);
		Test->AddCommand(new FPlayServAsyncStep(Test, TEXT("FreshPlayer"), [Test, Label, OutPlayerId, bDone]()
		{
			PlayServ::Auth::LoginAnonymous(MakeSuiteDisplayName(Label), FPlayServAuthCallback::CreateLambda([Test, OutPlayerId, bDone](bool bOk, const FString& PlayerId, const FPlayServError& Error)
			{
				if (!bOk)
				{
					Test->AddError(FString::Printf(TEXT("anonymous login failed: %s"), *Error.Message));
				}
				*OutPlayerId = PlayerId;
				*bDone = true;
			}));
		}, bDone, 8.0f));
	}

	template<typename T>
	void Record(TSharedPtr<FRowOutcome> Outcome, THolder<T> Holder, bool bOk, T* Row, bool bCreated, const FPlayServError& Error)
	{
		Outcome->bSuccess = bOk;
		Outcome->bCreated = bCreated;
		Outcome->Error = Error;
		Outcome->RecordId = Row != nullptr ? PlayServ::Data::GetRecordId(Row) : FString();
		*Holder = TStrongObjectPtr<T>(Row);
		Outcome->bDone = true;
	}

	template<typename T>
	void AddLoadPlayerOwnedStep(FAutomationTestBase* Test, const FString& StepName, TSharedPtr<FString> PlayerId, THolder<T> Holder, TSharedPtr<FRowOutcome> Outcome)
	{
		TSharedPtr<bool> bDone = MakeShared<bool>(false);
		Test->AddCommand(new FPlayServAsyncStep(Test, StepName, [bDone, PlayerId, Holder, Outcome]()
		{
			PlayServ::Data::LoadPlayerOwned<T>(*PlayerId, [bDone, Holder, Outcome](bool bOk, T* Row, bool bCreated, const FPlayServError& Error)
			{
				Record<T>(Outcome, Holder, bOk, Row, bCreated, Error);
				*bDone = true;
			});
		}, bDone, 12.0f));
	}

	template<typename T>
	void AddSaveStep(FAutomationTestBase* Test, const FString& StepName, THolder<T> Holder, TFunction<void(T*)> Change, TSharedPtr<FRowOutcome> Outcome)
	{
		TSharedPtr<bool> bDone = MakeShared<bool>(false);
		Test->AddCommand(new FPlayServAsyncStep(Test, StepName, [Test, StepName, bDone, Holder, Change, Outcome]()
		{
			if (!Holder->IsValid())
			{
				Test->AddError(FString::Printf(TEXT("[%s] no row to save"), *StepName));
				*bDone = true;
				return;
			}
			Change(Holder->Get());
			PlayServ::Data::Save(Holder->Get(), FPlayServSimpleCallback::CreateLambda([bDone, Outcome](bool bOk, const FPlayServError& Error)
			{
				Outcome->bSuccess = bOk;
				Outcome->Error = Error;
				Outcome->bDone = true;
				*bDone = true;
			}));
		}, bDone, 8.0f));
	}
}

// ---------------------------------------------------------------------------
// PlayServ.Data.PlayerOwned.ClientCreatesItsRowOnce
//
// A fresh player's first LoadPlayerOwned creates the row, and the platform row carries the player in player_id; the
// second load finds the same row; a Save on it lands and the next load reads it back. The first load goes through
// the controller overload with the local controller, the later ones through the id overload.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServPlayerOwnedClientCreatesItsRowOnceTest,
	"PlayServ.Data.PlayerOwned.ClientCreatesItsRowOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServPlayerOwnedClientCreatesItsRowOnceTest::RunTest(const FString& Parameters)
{
	using namespace PlayServPlayerOwnedTest;

	AddCommand(new FPlayServSetupStep(this));
	TSharedPtr<FString> PlayerId = MakeShared<FString>();
	AddFreshPlayerStep(this, TEXT("PlayerOwned.ClientCreatesItsRowOnce"), PlayerId);

	THolder<UTestPlayerProfile> First = MakeShared<TStrongObjectPtr<UTestPlayerProfile>>();
	TSharedPtr<FRowOutcome> FirstLoad = MakeShared<FRowOutcome>();
	TSharedPtr<bool> bFirstDone = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("LoadByController"), [bFirstDone, First, FirstLoad]()
	{
		APlayerController* Local = NewObject<APlayerController>(GetTransientPackage());
		Local->SetAsLocalPlayerController();
		PlayServ::Data::LoadPlayerOwned<UTestPlayerProfile>(Local, [bFirstDone, First, FirstLoad](bool bOk, UTestPlayerProfile* Row, bool bCreated, const FPlayServError& Error)
		{
			Record<UTestPlayerProfile>(FirstLoad, First, bOk, Row, bCreated, Error);
			*bFirstDone = true;
		});
	}, bFirstDone, 12.0f));
	AddCommand(new FPlayServAssertStep(this, [First, FirstLoad](FAutomationTestBase* T)
	{
		T->TestTrue(FString::Printf(TEXT("the first load succeeds (%s %s)"), *FirstLoad->Error.ProblemCode, *FirstLoad->Error.Message), FirstLoad->bSuccess);
		T->TestTrue(TEXT("the first load created the row"), FirstLoad->bCreated);
		T->TestTrue(TEXT("the row has the platform's record id"), FirstLoad->RecordId.StartsWith(TEXT("rec_")));
		if (First->IsValid())
		{
			T->TestEqual(TEXT("the row starts from the class defaults"), First->Get()->Coins, 0);
		}
	}));

	TSharedPtr<FString> EntId = MakeShared<FString>();
	PlayServWireTest::AddResolveEntityStep(this, TEXT("TestPlayerProfile"), EntId);
	PlayServWireTest::FWireResultPtr Stored = MakeShared<PlayServWireTest::FWireResult>();
	TSharedPtr<bool> bStoredFired = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("ReadStoredRow"), [EntId, FirstLoad, Stored, bStoredFired]()
	{
		PlayServWireTest::Fire(TEXT("GET"), FString::Printf(TEXT("/data/tables/%s/records/%s"), **EntId, *FirstLoad->RecordId), TEXT("player"), nullptr, Stored);
		*bStoredFired = true;
	}, bStoredFired));
	AddCommand(new FPlayServPollStep(this, TEXT("ReadStoredRowWait"), [Stored]() { return Stored->bCompleted; }, 15.0f));
	AddCommand(new FPlayServAssertStep(this, [Stored, PlayerId](FAutomationTestBase* T)
	{
		FString StoredPlayer;
		T->TestTrue(FString::Printf(TEXT("the platform row carries player_id (status %d)"), Stored->Status), Stored->Json.IsValid() && Stored->Json->TryGetStringField(TEXT("player_id"), StoredPlayer));
		T->TestEqual(TEXT("player_id is the signed-in player"), StoredPlayer, *PlayerId);
	}));

	THolder<UTestPlayerProfile> Second = MakeShared<TStrongObjectPtr<UTestPlayerProfile>>();
	TSharedPtr<FRowOutcome> SecondLoad = MakeShared<FRowOutcome>();
	AddLoadPlayerOwnedStep<UTestPlayerProfile>(this, TEXT("LoadAgain"), PlayerId, Second, SecondLoad);
	AddCommand(new FPlayServAssertStep(this, [FirstLoad, SecondLoad](FAutomationTestBase* T)
	{
		T->TestTrue(TEXT("the second load succeeds"), SecondLoad->bSuccess);
		T->TestFalse(TEXT("the second load did not create"), SecondLoad->bCreated);
		T->TestEqual(TEXT("both loads return the same row"), SecondLoad->RecordId, FirstLoad->RecordId);
	}));

	TSharedPtr<FRowOutcome> Saved = MakeShared<FRowOutcome>();
	AddSaveStep<UTestPlayerProfile>(this, TEXT("Save"), Second, [](UTestPlayerProfile* Row)
	{
		Row->Coins = 25;
		Row->Nickname = TEXT("PlayerOwned");
	}, Saved);

	THolder<UTestPlayerProfile> Third = MakeShared<TStrongObjectPtr<UTestPlayerProfile>>();
	TSharedPtr<FRowOutcome> ThirdLoad = MakeShared<FRowOutcome>();
	AddLoadPlayerOwnedStep<UTestPlayerProfile>(this, TEXT("LoadAfterSave"), PlayerId, Third, ThirdLoad);
	AddCommand(new FPlayServAssertStep(this, [Saved, Third, ThirdLoad](FAutomationTestBase* T)
	{
		T->TestTrue(FString::Printf(TEXT("a Save on the row succeeds (%s)"), *Saved->Error.Message), Saved->bSuccess);
		T->TestTrue(TEXT("the load after the Save succeeds"), ThirdLoad->bSuccess);
		if (Third->IsValid())
		{
			T->TestEqual(TEXT("the saved Coins are read back"), Third->Get()->Coins, 25);
			T->TestEqual(TEXT("the saved Nickname is read back"), Third->Get()->Nickname, FString(TEXT("PlayerOwned")));
		}
	}));
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Data.PlayerOwned.ClientRefusedAnotherPlayer
//
// On a client the row is the signed-in player's: another player's id and a null controller are refused before any
// request.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServPlayerOwnedClientRefusedAnotherPlayerTest,
	"PlayServ.Data.PlayerOwned.ClientRefusedAnotherPlayer",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServPlayerOwnedClientRefusedAnotherPlayerTest::RunTest(const FString& Parameters)
{
	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("PlayerOwned.ClientRefusedAnotherPlayer"));

	AddCommand(new FPlayServAssertStep(this, [](FAutomationTestBase* T)
	{
		bool bOtherAnswered = false;
		bool bOtherOk = true;
		PlayServ::Data::LoadPlayerOwned<UTestPlayerProfile>(PlayServPlayerOwnedTest::SyntheticPlayerId(), [&](bool bOk, UTestPlayerProfile*, bool, const FPlayServError&)
		{
			bOtherAnswered = true;
			bOtherOk = bOk;
		});
		T->TestTrue(TEXT("another player's id is answered at once"), bOtherAnswered);
		T->TestFalse(TEXT("another player's id is refused on a client"), bOtherOk);

		bool bNullOk = true;
		PlayServ::Data::LoadPlayerOwned<UTestPlayerProfile>(static_cast<const APlayerController*>(nullptr), [&](bool bOk, UTestPlayerProfile*, bool, const FPlayServError&) { bNullOk = bOk; });
		T->TestFalse(TEXT("a null controller is refused"), bNullOk);
	}));
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Data.PlayerOwned.ConcurrentFirstLoadsConverge
//
// Two first loads for one player, in flight together, return the same row, and exactly one of them created it.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServPlayerOwnedConcurrentFirstLoadsConvergeTest,
	"PlayServ.Data.PlayerOwned.ConcurrentFirstLoadsConverge",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServPlayerOwnedConcurrentFirstLoadsConvergeTest::RunTest(const FString& Parameters)
{
	using namespace PlayServPlayerOwnedTest;

	AddCommand(new FPlayServSetupStep(this));
	TSharedPtr<FString> PlayerId = MakeShared<FString>();
	AddFreshPlayerStep(this, TEXT("PlayerOwned.ConcurrentFirstLoadsConverge"), PlayerId);

	THolder<UTestPlayerProfile> A = MakeShared<TStrongObjectPtr<UTestPlayerProfile>>();
	THolder<UTestPlayerProfile> B = MakeShared<TStrongObjectPtr<UTestPlayerProfile>>();
	TSharedPtr<FRowOutcome> LoadA = MakeShared<FRowOutcome>();
	TSharedPtr<FRowOutcome> LoadB = MakeShared<FRowOutcome>();
	TSharedPtr<bool> bFired = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("TwoLoads"), [bFired, PlayerId, A, B, LoadA, LoadB]()
	{
		PlayServ::Data::LoadPlayerOwned<UTestPlayerProfile>(*PlayerId, [A, LoadA](bool bOk, UTestPlayerProfile* Row, bool bCreated, const FPlayServError& Error)
		{
			Record<UTestPlayerProfile>(LoadA, A, bOk, Row, bCreated, Error);
		});
		PlayServ::Data::LoadPlayerOwned<UTestPlayerProfile>(*PlayerId, [B, LoadB](bool bOk, UTestPlayerProfile* Row, bool bCreated, const FPlayServError& Error)
		{
			Record<UTestPlayerProfile>(LoadB, B, bOk, Row, bCreated, Error);
		});
		*bFired = true;
	}, bFired));
	AddCommand(new FPlayServPollStep(this, TEXT("TwoLoadsWait"), [LoadA, LoadB]() { return LoadA->bDone && LoadB->bDone; }, 15.0f));
	AddCommand(new FPlayServAssertStep(this, [LoadA, LoadB](FAutomationTestBase* T)
	{
		T->TestTrue(FString::Printf(TEXT("the first concurrent load succeeds (%s)"), *LoadA->Error.Message), LoadA->bSuccess);
		T->TestTrue(FString::Printf(TEXT("the second concurrent load succeeds (%s)"), *LoadB->Error.Message), LoadB->bSuccess);
		T->TestEqual(TEXT("both return the same row"), LoadA->RecordId, LoadB->RecordId);
		T->TestTrue(TEXT("exactly one of them created it"), LoadA->bCreated != LoadB->bCreated);
	}));
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Data.PlayerOwned.SaveRefusesAnInstanceThatIsNotAPlayersRow
//
// A player-owned row is created only through LoadPlayerOwned, which writes its player_id. An instance made any
// other way has no row, and Save refuses it rather than sending a create the table cannot accept.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServPlayerOwnedSaveRefusesAnInstanceThatIsNotAPlayersRowTest,
	"PlayServ.Data.PlayerOwned.SaveRefusesAnInstanceThatIsNotAPlayersRow",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServPlayerOwnedSaveRefusesAnInstanceThatIsNotAPlayersRowTest::RunTest(const FString& Parameters)
{
	using namespace PlayServPlayerOwnedTest;

	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("PlayerOwned.SaveRefusesAnInstanceThatIsNotAPlayersRow"));

	THolder<UTestPlayerProfile> Loose = MakeShared<TStrongObjectPtr<UTestPlayerProfile>>();
	TSharedPtr<FRowOutcome> Saved = MakeShared<FRowOutcome>();
	AddCommand(new FPlayServAssertStep(this, [Loose](FAutomationTestBase*)
	{
		*Loose = TStrongObjectPtr<UTestPlayerProfile>(NewObject<UTestPlayerProfile>(GetTransientPackage()));
	}));
	AddSaveStep<UTestPlayerProfile>(this, TEXT("SaveLoose"), Loose, [](UTestPlayerProfile* Row) { Row->Coins = 5; }, Saved);
	AddCommand(new FPlayServAssertStep(this, [Saved, Loose](FAutomationTestBase* T)
	{
		T->TestFalse(TEXT("the Save is refused"), Saved->bSuccess);
		T->TestTrue(FString::Printf(TEXT("the refusal points to LoadPlayerOwned (%s)"), *Saved->Error.Message), Saved->Error.Message.Contains(TEXT("LoadPlayerOwned")));
		T->TestTrue(TEXT("nothing was created"), Loose->IsValid() && PlayServ::Data::GetRecordId(Loose->Get()).IsEmpty());
	}));
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Data.PlayerOwned.ClientCannotCreateInAClosedTable
//
// A table closed to client writes holds rows only servers write. A client's first load finds no row and cannot
// create one: NotFound, with the platform's reason, and no instance.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServPlayerOwnedClientCannotCreateInAClosedTableTest,
	"PlayServ.Data.PlayerOwned.ClientCannotCreateInAClosedTable",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServPlayerOwnedClientCannotCreateInAClosedTableTest::RunTest(const FString& Parameters)
{
	using namespace PlayServPlayerOwnedTest;

	AddCommand(new FPlayServSetupStep(this));
	TSharedPtr<FString> PlayerId = MakeShared<FString>();
	AddFreshPlayerStep(this, TEXT("PlayerOwned.ClientCannotCreateInAClosedTable"), PlayerId);

	THolder<UTestPlayerStanding> Row = MakeShared<TStrongObjectPtr<UTestPlayerStanding>>();
	TSharedPtr<FRowOutcome> Refused = MakeShared<FRowOutcome>();
	AddLoadPlayerOwnedStep<UTestPlayerStanding>(this, TEXT("ClientLoadClosed"), PlayerId, Row, Refused);
	AddCommand(new FPlayServAssertStep(this, [Row, Refused](FAutomationTestBase* T)
	{
		T->TestFalse(TEXT("a client does not get a row it may not create"), Refused->bSuccess);
		T->TestFalse(TEXT("nothing is reported created"), Refused->bCreated);
		T->TestFalse(TEXT("no instance is handed back"), Row->IsValid());
		T->TestEqual(TEXT("the missing row is NotFound"), Refused->Error.Code, EPlayServErrorCode::NotFound);
		T->TestEqual(TEXT("the platform's reason is kept"), Refused->Error.ProblemCode, FString(TEXT("table_write_forbidden")));
	}));
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Data.PlayerOwned.ServerDoesNotCreate
//
// A dedicated server loads and saves player-owned rows but does not create them: a player with no row yet gets
// NotFound, without the SDK asking the platform to create one.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServPlayerOwnedServerDoesNotCreateTest,
	"PlayServ.Data.PlayerOwned.ServerDoesNotCreate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServPlayerOwnedServerDoesNotCreateTest::RunTest(const FString& Parameters)
{
	using namespace PlayServPlayerOwnedTest;

	AddCommand(new FPlayServSetupStep(this));
	PlayServWireTest::AddServerLoginStep(this);
	TSharedPtr<FString> PlayerId = MakeShared<FString>(SyntheticPlayerId());

	THolder<UTestPlayerProfile> Row = MakeShared<TStrongObjectPtr<UTestPlayerProfile>>();
	TSharedPtr<FRowOutcome> Missing = MakeShared<FRowOutcome>();
	AddLoadPlayerOwnedStep<UTestPlayerProfile>(this, TEXT("ServerLoadMissing"), PlayerId, Row, Missing);
	AddCommand(new FPlayServAssertStep(this, [Row, Missing](FAutomationTestBase* T)
	{
		T->TestFalse(TEXT("a server gets no row that does not exist"), Missing->bSuccess);
		T->TestFalse(TEXT("nothing is reported created"), Missing->bCreated);
		T->TestFalse(TEXT("no instance is handed back"), Row->IsValid());
		T->TestEqual(TEXT("the missing row is NotFound"), Missing->Error.Code, EPlayServErrorCode::NotFound);
		T->TestTrue(TEXT("no create was asked of the platform"), Missing->Error.ProblemCode.IsEmpty());
	}));
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Data.PlayerOwned.ServerLoadsTheRowTheClientMade
//
// The player's session creates its own row; a server session then finds that row by the player's id and saves it.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServPlayerOwnedServerLoadsTheRowTheClientMadeTest,
	"PlayServ.Data.PlayerOwned.ServerLoadsTheRowTheClientMade",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServPlayerOwnedServerLoadsTheRowTheClientMadeTest::RunTest(const FString& Parameters)
{
	using namespace PlayServPlayerOwnedTest;

	AddCommand(new FPlayServSetupStep(this));
	TSharedPtr<FString> PlayerId = MakeShared<FString>();
	AddFreshPlayerStep(this, TEXT("PlayerOwned.ServerLoadsTheRowTheClientMade"), PlayerId);

	THolder<UTestPlayerProfile> ClientRow = MakeShared<TStrongObjectPtr<UTestPlayerProfile>>();
	TSharedPtr<FRowOutcome> ClientLoad = MakeShared<FRowOutcome>();
	AddLoadPlayerOwnedStep<UTestPlayerProfile>(this, TEXT("ClientCreate"), PlayerId, ClientRow, ClientLoad);

	PlayServWireTest::AddServerLoginStep(this);

	THolder<UTestPlayerProfile> ServerRow = MakeShared<TStrongObjectPtr<UTestPlayerProfile>>();
	TSharedPtr<FRowOutcome> ServerLoad = MakeShared<FRowOutcome>();
	AddLoadPlayerOwnedStep<UTestPlayerProfile>(this, TEXT("ServerFind"), PlayerId, ServerRow, ServerLoad);
	AddCommand(new FPlayServAssertStep(this, [ClientLoad, ServerLoad](FAutomationTestBase* T)
	{
		T->TestTrue(FString::Printf(TEXT("the player creates its row (%s)"), *ClientLoad->Error.Message), ClientLoad->bSuccess && ClientLoad->bCreated);
		T->TestTrue(FString::Printf(TEXT("a server finds the player's row (%s)"), *ServerLoad->Error.Message), ServerLoad->bSuccess);
		T->TestFalse(TEXT("the server did not create another"), ServerLoad->bCreated);
		T->TestEqual(TEXT("it is the row the player made"), ServerLoad->RecordId, ClientLoad->RecordId);
	}));

	TSharedPtr<FRowOutcome> ServerSave = MakeShared<FRowOutcome>();
	AddSaveStep<UTestPlayerProfile>(this, TEXT("ServerSave"), ServerRow, [](UTestPlayerProfile* Row) { Row->Coins = 99; }, ServerSave);
	AddCommand(new FPlayServAssertStep(this, [ServerSave](FAutomationTestBase* T)
	{
		T->TestTrue(FString::Printf(TEXT("a server saves the player's row (%s)"), *ServerSave->Error.Message), ServerSave->bSuccess);
	}));
	return true;
}

// ---------------------------------------------------------------------------
// PlayServ.Data.PlayerOwned.ServerRefusesARowAnotherPlayerOwns
//
// A client going around the SDK can write any player_id into its own row of a client-writable table. A row carrying
// player X but owned by player Y is not X's: a server loading X's row is refused it rather than handed Y's data.
// ---------------------------------------------------------------------------

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FPlayServPlayerOwnedServerRefusesARowAnotherPlayerOwnsTest,
	"PlayServ.Data.PlayerOwned.ServerRefusesARowAnotherPlayerOwns",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)

bool FPlayServPlayerOwnedServerRefusesARowAnotherPlayerOwnsTest::RunTest(const FString& Parameters)
{
	using namespace PlayServPlayerOwnedTest;

	AddExpectedError(TEXT("owned by another"), EAutomationExpectedErrorFlags::Contains, 1);

	AddCommand(new FPlayServSetupStep(this));
	AddLoginStep(this, TEXT("PlayerOwned.ServerRefusesARowAnotherPlayerOwns"));
	TSharedPtr<FString> EntId = MakeShared<FString>();
	PlayServWireTest::AddResolveEntityStep(this, TEXT("TestPlayerProfile"), EntId);

	TSharedPtr<FString> VictimId = MakeShared<FString>(SyntheticPlayerId());
	PlayServWireTest::FWireResultPtr Squat = MakeShared<PlayServWireTest::FWireResult>();
	TSharedPtr<bool> bSquatFired = MakeShared<bool>(false);
	AddCommand(new FPlayServAsyncStep(this, TEXT("SquatThePlayerId"), [EntId, VictimId, Squat, bSquatFired]()
	{
		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		Body->SetStringField(TEXT("player_id"), *VictimId);
		Body->SetNumberField(TEXT("Coins"), 1000000);
		PlayServWireTest::Fire(TEXT("POST"), FString::Printf(TEXT("/data/tables/%s/records"), **EntId), TEXT("player"), Body, Squat);
		*bSquatFired = true;
	}, bSquatFired));
	AddCommand(new FPlayServPollStep(this, TEXT("SquatThePlayerIdWait"), [Squat]() { return Squat->bCompleted; }, 15.0f));
	AddCommand(new FPlayServAssertStep(this, [Squat](FAutomationTestBase* T)
	{
		T->TestEqual(TEXT("precondition: a client made a row carrying another player"), Squat->Status, 201);
	}));

	PlayServWireTest::AddServerLoginStep(this);

	THolder<UTestPlayerProfile> Row = MakeShared<TStrongObjectPtr<UTestPlayerProfile>>();
	TSharedPtr<FRowOutcome> Refused = MakeShared<FRowOutcome>();
	AddLoadPlayerOwnedStep<UTestPlayerProfile>(this, TEXT("ServerLoadVictim"), VictimId, Row, Refused);
	AddCommand(new FPlayServAssertStep(this, [Row, Refused](FAutomationTestBase* T)
	{
		T->TestFalse(TEXT("a row another player owns is refused"), Refused->bSuccess);
		T->TestFalse(TEXT("no instance is handed back"), Row->IsValid());
	}));
	return true;
}

#endif // !UE_BUILD_SHIPPING
