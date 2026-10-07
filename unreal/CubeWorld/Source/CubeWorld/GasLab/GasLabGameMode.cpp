#include "GasLab/GasLabGameMode.h"
#include "GasLab/GasLabAbility.h"
#include "GasLab/GasLabAbilitySystemComponent.h"
#include "GasLab/GasLabAttributeSet.h"
#include "GasLab/GasLabBorder.h"
#include "GasLab/GasLabCharacter.h"
#include "GasLab/GasLabClock.h"
#include "GasLab/GasLabGameState.h"
#include "GasLab/GasLabPlayerController.h"
#include "GasLab/GasLabPlayerState.h"
#include "GasLab/GasLabTags.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GasLabGameMode)

namespace
{
	const FVector Corner(UGasLabBorder::RegionSize, UGasLabBorder::RegionSize, 100.f);
}

AGasLabGameMode::AGasLabGameMode()
{
	PrimaryActorTick.bCanEverTick = true;
	DefaultPawnClass = AGasLabCharacter::StaticClass();
	PlayerControllerClass = AGasLabPlayerController::StaticClass();
	PlayerStateClass = AGasLabPlayerState::StaticClass();
	GameStateClass = AGasLabGameState::StaticClass();
	bStartPlayersAsSpectators = false;
}

void AGasLabGameMode::BeginPlay()
{
	Super::BeginPlay();
	FParse::Value(FCommandLine::Get(), TEXT("-region="), Region);
	FParse::Value(FCommandLine::Get(), TEXT("-gasscenario="), ScenarioName);
	FParse::Value(FCommandLine::Get(), TEXT("-gasbots="), SoakBots);
	Random.Initialize(1000 + Region);
	// Scenario time starts once every neighbour is linked (Tick), so servers that boot at different speeds play together.
	StartedAt = -1.0;
	AGasLabFloor::Ensure(GetWorld());
	UE_LOG(LogTemp, Display, TEXT("GASLAB server region=%d scenario=%s at=%.3f"), Region, *ScenarioName, StartedAt);
}

FString AGasLabGameMode::InitNewPlayer(APlayerController* NewPlayerController, const FUniqueNetIdRepl& UniqueId, const FString& Options, const FString& Portal)
{
	const FString Result = Super::InitNewPlayer(NewPlayerController, UniqueId, Options, Portal);
	if (AGasLabPlayerController* PC = Cast<AGasLabPlayerController>(NewPlayerController))
	{
		PC->GasName = UGameplayStatics::ParseOption(Options, TEXT("gasname"));
		PC->GasId = UGameplayStatics::ParseOption(Options, TEXT("gasplayer"));
		if (PC->GasName.IsEmpty()) PC->GasName = TEXT("Player");
		if (PC->GasId.IsEmpty()) PC->GasId = PC->GasName.ToLower();
		PC->bGasCrossing = UGameplayStatics::HasOption(Options, TEXT("gascross"));
	}
	return Result;
}

void AGasLabGameMode::HandleStartingNewPlayer_Implementation(APlayerController* NewPlayer)
{
	// Not the stock restart: a player handed over waits for its state.
	StartPlayer(NewPlayer, false);
}

void AGasLabGameMode::StartPlayer(APlayerController* PC, bool bGiveUp)
{
	AGasLabPlayerController* Lab = Cast<AGasLabPlayerController>(PC);
	UGasLabBorder* Border = UGasLabBorder::Get(this);
	if (!Lab || !Border || Lab->GetPawn()) return;
	if (Lab->bGasCrossing && !Border->HasArrival(Lab->GasId) && !bGiveUp)
	{
		Waiting.Add(Lab, FPlatformTime::Seconds());
		UE_LOG(LogTemp, Log, TEXT("GASLAB arrival-waits id=%s at=%.3f"), *Lab->GasId, GasLabClock::Now());
		return;
	}
	Waiting.Remove(Lab);
	FGasLabWirePlayer State;
	const bool bHas = Border->TakeArrival(Lab->GasId, State);
	if (Lab->bGasCrossing && !bHas) UE_LOG(LogTemp, Warning, TEXT("GASLAB arrival-missing id=%s at=%.3f"), *Lab->GasId, GasLabClock::Now());
	FVector At = bHas ? State.Location : UGasLabBorder::RegionCenter(Region) + FVector(Random.FRandRange(-300.f, 300.f), Random.FRandRange(-300.f, 300.f), 0.f);
	// The client scenario puts a joining player 3.5 m from the border with region 1.
	if (!bHas && ScenarioName == TEXT("client") && Region == 0) At = FVector(2050.f, 1400.f, 100.f);
	// The corner scenario puts a joining player outside the blast, looking at the corner where the four servers meet.
	float Yaw = bHas ? State.Yaw : 0.f;
	if (!bHas && ScenarioName == TEXT("corner") && Region == 0) { At = FVector(1700.f, 1820.f, 100.f); Yaw = 40.f; }
	SpawnFor(Lab, Lab->GasId, Lab->GasName, At, Yaw, bHas ? &State : nullptr);
}

void AGasLabGameMode::ArrivalReady(const FString& Id)
{
	for (auto It = Waiting.CreateIterator(); It; ++It)
	{
		AGasLabPlayerController* PC = Cast<AGasLabPlayerController>(It->Key.Get());
		if (PC && PC->GasId == Id) { StartPlayer(PC, false); return; }
	}
}

AGasLabCharacter* AGasLabGameMode::SpawnFor(AController* Controller, const FString& Id, const FString& Name, const FVector& At, float Yaw, const FGasLabWirePlayer* Restore)
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
	AGasLabCharacter* Character = GetWorld()->SpawnActor<AGasLabCharacter>(AGasLabCharacter::StaticClass(), FVector(At.X, At.Y, FMath::Max(At.Z, 100.f)), FRotator(0.f, Yaw, 0.f), Params);
	AGasLabPlayerState* PS = Controller ? Controller->GetPlayerState<AGasLabPlayerState>() : nullptr;
	if (!Character || !PS) { if (Character) Character->Destroy(); return nullptr; }
	PS->LabId = Id;
	PS->OwnerRegion = Region;
	PS->SetPlayerName(Name);
	UGasLabAbilitySystemComponent* ASC = PS->GetLabASC();
	ASC->PlayerId = Id;
	Controller->SetControlRotation(FRotator(0.f, Yaw, 0.f));
	Controller->Possess(Character);
	if (!ASC->FindAbilitySpecFromClass(UGasLabAbility_Throw::StaticClass()))
	{
		ASC->GiveAbility(FGameplayAbilitySpec(UGasLabAbility_Throw::StaticClass(), 1));
	}
	if (Restore)
	{
		UGasLabBorder::Get(this)->Restore(*ASC, *Restore);
		Character->GetCharacterMovement()->Velocity = Restore->Velocity;
	}
	UE_LOG(LogTemp, Log, TEXT("GASLAB spawn id=%s region=%d restored=%d bot=%d pos=%.0f,%.0f at=%.3f"), *Id, Region, Restore ? 1 : 0, Controller->IsPlayerController() ? 0 : 1, At.X, At.Y, GasLabClock::Now());
	return Character;
}

AGasLabCharacter* AGasLabGameMode::SpawnBot(const FString& Id, const FString& Name, const FVector& At, float Yaw, const FGasLabWirePlayer* Restore)
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AGasLabBotController* Bot = GetWorld()->SpawnActor<AGasLabBotController>(AGasLabBotController::StaticClass(), FTransform::Identity, Params);
	if (!Bot) return nullptr;
	AGasLabCharacter* Character = SpawnFor(Bot, Id, Name, At, Yaw, Restore);
	if (!Character) { Bot->Destroy(); return nullptr; }
	return Character;
}

void AGasLabGameMode::Logout(AController* Exiting)
{
	if (APawn* Pawn = Exiting ? Exiting->GetPawn() : nullptr) Pawn->Destroy();
	Super::Logout(Exiting);
}

void AGasLabGameMode::ThrowFrom(const FString& Id, const FVector& Target)
{
	UGasLabBorder* Border = UGasLabBorder::Get(this);
	AGasLabCharacter* Thrower = Border ? Border->FindCharacter(Id, false) : nullptr;
	UGasLabAbilitySystemComponent* ASC = Thrower ? Thrower->GetLabASC() : nullptr;
	if (!ASC) return;
	Thrower->ThrowTarget = Target;
	const bool bOk = ASC->TryActivateAbilityByClass(UGasLabAbility_Throw::StaticClass());
	UE_LOG(LogTemp, Log, TEXT("GASLAB bot-throw id=%s ok=%d cooldown=%.3f at=%.3f"), *Id, bOk ? 1 : 0, ASC->TimeLeft(GasLabTags::Cooldown_Throw), GasLabClock::Now());
}

void AGasLabGameMode::WalkBot(const FString& Id, const FVector& To)
{
	UGasLabBorder* Border = UGasLabBorder::Get(this);
	AGasLabCharacter* Who = Border ? Border->FindCharacter(Id, false) : nullptr;
	if (AGasLabBotController* Bot = Who ? Cast<AGasLabBotController>(Who->GetController()) : nullptr) Bot->WalkTo = To;
}

void AGasLabGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	for (auto It = Waiting.CreateIterator(); It; ++It)
	{
		APlayerController* PC = It->Key.Get();
		if (!PC) { It.RemoveCurrent(); continue; }
		if (FPlatformTime::Seconds() - It->Value > 5.0) { APlayerController* Late = PC; It.RemoveCurrent(); StartPlayer(Late, true); }
	}
	if (StartedAt < 0.0)
	{
		UGasLabBorder* Border = UGasLabBorder::Get(this);
		// Region 0 picks the start two seconds after every link is up and tells the others, so all play one timeline.
		if (Border && Region == 0 && Border->GetScenarioStart() < 0.0 && Border->AllConnected()) Border->AnnounceStart(GasLabClock::Now() + 2.0);
		if (Border && Border->GetScenarioStart() >= 0.0)
		{
			StartedAt = Border->GetScenarioStart();
			UE_LOG(LogTemp, Display, TEXT("GASLAB scenario-start scenario=%s start=%.3f at=%.3f"), *ScenarioName, StartedAt, GasLabClock::Now());
		}
	}
	// Over the relay a server that joins it late misses the first announcement: region 0 repeats it for a while.
	if (Region == 0 && StartedAt >= 0.0 && GasLabClock::Now() < StartedAt + 8.0 && GasLabClock::Now() >= NextAnnounce)
	{
		NextAnnounce = GasLabClock::Now() + 1.0;
		if (UGasLabBorder* Border = UGasLabBorder::Get(this)) Border->AnnounceStart(StartedAt);
	}
	if (!ScenarioName.IsEmpty() && StartedAt >= 0.0 && GasLabClock::Now() >= StartedAt) Scenario(float(GasLabClock::Now() - StartedAt));
	if (GasLabClock::Now() >= NextStateLog && ScenarioName != TEXT("soak"))
	{
		NextStateLog = GasLabClock::Now() + 0.1;
		LogStates();
	}
}

void AGasLabGameMode::LogStates()
{
	for (TActorIterator<AGasLabCharacter> It(GetWorld()); It; ++It)
	{
		UGasLabAbilitySystemComponent* ASC = It->GetLabASC();
		if (It->IsGhost() || !ASC) continue;
		const FVector P = It->GetActorLocation();
		UE_LOG(LogTemp, Log, TEXT("GASLAB state id=%s region=%d hp=%.2f burnleft=%.3f stacks=%d nexttick=%.3f cooldown=%.3f dealt=%.2f pos=%.0f,%.0f at=%.3f"),
			*ASC->PlayerId, Region, ASC->GetNumericAttribute(UGasLabAttributeSet::GetHealthAttribute()), ASC->TimeLeft(GasLabTags::Effect_Burning),
			ASC->Stacks(GasLabTags::Effect_Burning), ASC->NextTickIn(GasLabTags::Effect_Burning), ASC->TimeLeft(GasLabTags::Cooldown_Throw), ASC->DamageDealt, P.X, P.Y, GasLabClock::Now());
	}
}

void AGasLabGameMode::Scenario(float T)
{
	auto Once = [this, T](const TCHAR* Step, float At) { if (T < At || Done.Contains(Step)) return false; Done.Add(Step); return true; };
	const FVector Center = UGasLabBorder::RegionCenter(Region);
	// Which way the corner is from this region: +1 or -1 per axis.
	const FVector Toward((Region % UGasLabBorder::Columns) == 0 ? 1.f : -1.f, (Region / UGasLabBorder::Columns) == 0 ? 1.f : -1.f, 0.f);

	if (ScenarioName == TEXT("corner"))
	{
		if (Once(TEXT("spawn"), 1.0f))
		{
			SpawnBot(FString::Printf(TEXT("near%d"), Region), FString::Printf(TEXT("Near%d"), Region), Corner - Toward * FVector(150.f, 170.f, 0.f), 0.f, nullptr);
			SpawnBot(FString::Printf(TEXT("mid%d"), Region), FString::Printf(TEXT("Mid%d"), Region), Corner - Toward * FVector(420.f, 260.f, 0.f), 0.f, nullptr);
			if (Region == 0)
			{
				AGasLabCharacter* Thrower = SpawnBot(TEXT("thrower"), TEXT("Thrower"), Center, 45.f, nullptr);
				// The blast snapshots the thrower's BlastPower: 1.5 makes the snapshot visible in every server's damage.
				if (Thrower && Thrower->GetLabASC()) Thrower->GetLabASC()->SetNumericAttributeBase(UGasLabAttributeSet::GetBlastPowerAttribute(), 1.5f);
			}
		}
		if (Region == 0)
		{
			if (Once(TEXT("throw1"), 6.f)) ThrowFrom(TEXT("thrower"), Corner);
			if (Once(TEXT("throw2"), 14.f)) ThrowFrom(TEXT("thrower"), Corner);
			if (Once(TEXT("throw3"), 22.f)) ThrowFrom(TEXT("thrower"), Corner);
		}
	}
	else if (ScenarioName == TEXT("cross"))
	{
		if (Region == 0)
		{
			if (Once(TEXT("spawn"), 1.0f))
			{
				AGasLabCharacter* Thrower = SpawnBot(TEXT("thrower"), TEXT("Thrower"), FVector(1300.f, 500.f, 100.f), 0.f, nullptr);
				if (Thrower && Thrower->GetLabASC()) Thrower->GetLabASC()->SetNumericAttributeBase(UGasLabAttributeSet::GetBlastPowerAttribute(), 1.5f);
				// Armor 100 halves the blast: the execution captures it live on the target's own server.
				AGasLabCharacter* Walker = SpawnBot(TEXT("walker"), TEXT("Walker"), FVector(2050.f, 1400.f, 100.f), 0.f, nullptr);
				if (Walker && Walker->GetLabASC()) Walker->GetLabASC()->SetNumericAttributeBase(UGasLabAttributeSet::GetArmorAttribute(), 100.f);
			}
			if (Once(TEXT("throw1"), 4.f)) ThrowFrom(TEXT("thrower"), FVector(2050.f, 1400.f, 0.f));
			if (Once(TEXT("throw2"), 9.f)) ThrowFrom(TEXT("thrower"), FVector(2050.f, 1400.f, 0.f));
			if (Once(TEXT("thrower-walks"), 9.2f)) WalkBot(TEXT("thrower"), FVector(2800.f, 400.f, 0.f));
			// After the second blast (9 + 2.2 s): the walker crosses burning with two stacks.
			if (Once(TEXT("walker-walks"), 11.5f)) WalkBot(TEXT("walker"), FVector(3200.f, 1400.f, 0.f));
		}
		if (Region == 1 && Once(TEXT("throw3"), 15.f))
		{
			UGasLabBorder* Border = UGasLabBorder::Get(this);
			AGasLabCharacter* Walker = Border ? Border->FindCharacter(TEXT("walker"), false) : nullptr;
			ThrowFrom(TEXT("thrower"), Walker ? Walker->GetActorLocation() : FVector(3200.f, 1400.f, 0.f));
		}
	}
	else if (ScenarioName == TEXT("client"))
	{
		// A client player "ann" stands near the border: it throws (predicted), gets burned twice, then walks over to
		// region 1 burning, with its cooldown running, through a server switch.
		UGasLabBorder* Border = UGasLabBorder::Get(this);
		AGasLabCharacter* Ann = Border ? Border->FindCharacter(TEXT("ann"), false) : nullptr;
		AGasLabPlayerController* AnnPC = Ann ? Cast<AGasLabPlayerController>(Ann->GetController()) : nullptr;
		if (Region == 0)
		{
			if (Once(TEXT("spawn"), 1.0f))
			{
				AGasLabCharacter* Thrower = SpawnBot(TEXT("thrower"), TEXT("Thrower"), FVector(1300.f, 500.f, 100.f), 0.f, nullptr);
				if (Thrower && Thrower->GetLabASC()) Thrower->GetLabASC()->SetNumericAttributeBase(UGasLabAttributeSet::GetBlastPowerAttribute(), 1.5f);
			}
			if (Ann && Ann->GetLabASC() && Once(TEXT("armor"), 1.5f)) Ann->GetLabASC()->SetNumericAttributeBase(UGasLabAttributeSet::GetArmorAttribute(), 100.f);
			if (AnnPC && Once(TEXT("ann-throw1"), 3.f)) AnnPC->ClientGasThrow();
			if (Ann && Once(TEXT("throw1"), 4.f)) ThrowFrom(TEXT("thrower"), Ann->GetActorLocation());
			if (Ann && Once(TEXT("throw2"), 9.f)) ThrowFrom(TEXT("thrower"), Ann->GetActorLocation());
			if (AnnPC && Once(TEXT("ann-throw2"), 9.5f)) AnnPC->ClientGasThrow();
			if (AnnPC && Once(TEXT("ann-walks"), 11.5f)) AnnPC->ClientGasWalk(FVector(3200.f, 1400.f, 0.f));
		}
	}
	else if (ScenarioName == TEXT("soak"))
	{
		if (Once(TEXT("spawn"), 1.0f))
		{
			for (int32 I = 0; I < SoakBots; I++)
			{
				const FBox2D Box = UGasLabBorder::RegionBox(Region);
				const FVector At(Random.FRandRange(Box.Min.X + 100, Box.Max.X - 100), Random.FRandRange(Box.Min.Y + 100, Box.Max.Y - 100), 100.f);
				SpawnBot(FString::Printf(TEXT("s%d-%d"), Region, I), FString::Printf(TEXT("S%d-%d"), Region, I), At, 0.f, nullptr);
			}
		}
		const double Now = GasLabClock::Now();
		if (T > 3.f && Now >= NextSoak)
		{
			NextSoak = Now + 0.5;
			const float World = UGasLabBorder::RegionSize * UGasLabBorder::Columns;
			TArray<AGasLabCharacter*> Mine, Everyone;
			for (TActorIterator<AGasLabCharacter> It(GetWorld()); It; ++It)
			{
				Everyone.Add(*It);
				if (!It->IsGhost()) Mine.Add(*It);
			}
			for (AGasLabCharacter* C : Mine)
			{
				AGasLabBotController* Bot = Cast<AGasLabBotController>(C->GetController());
				if (!Bot) continue;
				if (!Bot->WalkTo.IsSet()) Bot->WalkTo = FVector(Random.FRandRange(200.f, World - 200.f), Random.FRandRange(200.f, World - 200.f), 0.f);
				if (Random.FRand() < 0.15f && Everyone.Num() > 1)
				{
					AGasLabCharacter* Target = Everyone[Random.RandRange(0, Everyone.Num() - 1)];
					if (Target != C && FVector::Dist2D(Target->GetActorLocation(), C->GetActorLocation()) < 1400.f) ThrowFrom(C->GetPlayerId(), Target->GetActorLocation());
				}
			}
		}
	}
}
