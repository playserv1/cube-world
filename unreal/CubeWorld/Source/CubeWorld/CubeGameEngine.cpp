#include "CubeGameEngine.h"
#include "CubeWorld.h"
#include "Engine/LocalPlayer.h"
#include "Engine/NetDriver.h"
#include "Engine/PendingNetGame.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "UObject/UnrealType.h"

UCubeGameEngine* UCubeGameEngine::Get()
{
	return Cast<UCubeGameEngine>(GEngine);
}

void UCubeGameEngine::RequestSeamlessTravel()
{
	bSeamlessRequested = true;
	SeamlessStartedAt = FPlatformTime::Seconds();
	SeamlessFailure.Empty();
}

void UCubeGameEngine::Tick(float DeltaSeconds, bool bIdleMode)
{
	Super::Tick(DeltaSeconds, bIdleMode);
	if (!bSeamlessRequested) return;
	if (SeamlessFailure.IsEmpty() && FPlatformTime::Seconds() - SeamlessStartedAt > SeamlessTimeoutSeconds)
		SeamlessFailure = FString::Printf(TEXT("no answer in %.0f s"), SeamlessTimeoutSeconds);
	if (SeamlessFailure.IsEmpty()) return;
	// Out here, not in the failure's handler: that runs inside the handshake driver's own tick, which must not be
	// destroyed under it.
	const FString Why = MoveTemp(SeamlessFailure);
	SeamlessFailure.Empty();
	bSeamlessRequested = false;
	for (FWorldContext& Context : WorldList) if (Context.PendingNetGame) CancelPending(Context);
	UE_LOG(LogCubeWorld, Log, TEXT("seamless travel: the next server did not let us in (%s); staying where we are"), *Why);
	OnSeamlessTravelFailed.Broadcast(Why);
}

bool UCubeGameEngine::ShouldShutdownWorldNetDriver()
{
	// Browse would close the old connection before the next server is asked; a seamless travel keeps it playing.
	return !bSeamlessRequested && Super::ShouldShutdownWorldNetDriver();
}

void UCubeGameEngine::HandleNetworkFailure(UWorld* World, UNetDriver* NetDriver, ENetworkFailure::Type FailureType, const FString& ErrorString)
{
	// The next server's handshake failed (refused, timed out, lost): only the travel is over, and the old connection,
	// never closed, still plays. The engine's own handling (this is its OnNetworkFailure handler) would disconnect from
	// it and browse to the default map: the world we are in is fine. The travel is given up on the next tick
	// (OnSeamlessTravelFailed), and the game instance tries again later.
	if (bSeamlessRequested && NetDriver)
		for (const FWorldContext& Context : WorldList)
			if (Context.PendingNetGame && Context.PendingNetGame->NetDriver == NetDriver)
			{
				if (SeamlessFailure.IsEmpty()) SeamlessFailure = FString::Printf(TEXT("%s: %s"), ENetworkFailure::ToString(FailureType), *ErrorString);
				return;
			}
	Super::HandleNetworkFailure(World, NetDriver, FailureType, ErrorString);
}

bool UCubeGameEngine::LoadMap(FWorldContext& WorldContext, FURL URL, UPendingNetGame* Pending, FString& Error)
{
	const bool bSeamless = bSeamlessRequested;
	bSeamlessRequested = false;
	UWorld* World = WorldContext.World();
	if (bSeamless && Pending && World && World->PersistentLevel
		&& UWorld::RemovePIEPrefix(URL.Map) == UWorld::RemovePIEPrefix(World->GetOutermost()->GetName()))
	{
		return AdoptPendingNetGame(WorldContext, URL, Pending, Error);
	}
	return Super::LoadMap(WorldContext, URL, Pending, Error);
}

bool UCubeGameEngine::AdoptPendingNetGame(FWorldContext& Context, const FURL& URL, UPendingNetGame* Pending, FString& Error)
{
	UWorld* World = Context.World();
	const double Started = FPlatformTime::Seconds();
	UE_LOG(LogCubeWorld, Log, TEXT("seamless travel: %s takes over this world, which stays loaded"), *URL.ToString());

	// The old server's actors go with its connection: its controller, the pawn, the game and player states, the world's
	// state actor. The client's own actors stay: the drawn world, the other players' models, the bombs, the view.
	ClearServerActors(World);
	ShutdownWorldNetDriver(World);
	// Whatever the shutdown left behind of the old server's.
	ClearServerActors(World);
	DropLocalGameMode(World);

	// The handshake's connection becomes the world's, as LoadMap hands it to a freshly loaded world.
	check(Pending == Context.PendingNetGame);
	MovePendingLevel(Context);
	Context.LastURL = URL;
	Context.LastURL.Map = UWorld::RemovePIEPrefix(URL.Map);
	Context.LastRemoteURL = URL;
	UE_LOG(LogCubeWorld, Log, TEXT("seamless travel: the world changed hands in %.1f ms; joining"), (FPlatformTime::Seconds() - Started) * 1000.0);

	OnServerSwitched.Broadcast(World, true);
	// TickWorldTravel goes on as after any LoadMap: LoadMapCompleted, then TravelCompleted sends the join.
	return true;
}

void UCubeGameEngine::DisconnectKeepWorld(UWorld* World)
{
	if (!World || !World->GetNetDriver()) return;
	UE_LOG(LogCubeWorld, Log, TEXT("leaving the Unreal server; the world stays as the client's own"));
	ClearServerActors(World);
	ShutdownWorldNetDriver(World);
	ClearServerActors(World);
	OnServerSwitched.Broadcast(World, false);
}

void UCubeGameEngine::ClearServerActors(UWorld* World)
{
	if (!World) return;
	// The local player's controller is the server's: a local one holds the view until the next server's arrives, and the
	// engine replaces it then (UNetConnection::HandleClientPlayer destroys an authority placeholder).
	for (FLocalPlayerIterator It(this, World); It; ++It)
	{
		ULocalPlayer* Player = *It;
		APlayerController* Old = Player->PlayerController;
		if (!Old || Old->GetLocalRole() == ROLE_Authority) continue;
		FVector Location; FRotator Rotation;
		Old->GetPlayerViewPoint(Location, Rotation);
		FActorSpawnParameters Params;
		Params.ObjectFlags |= RF_Transient;
		APlayerController* Local = World->SpawnActor<APlayerController>(APlayerController::StaticClass(), Location, Rotation, Params);
		Old->Player = nullptr;
		Player->PlayerController = nullptr;
		if (Local)
		{
			Local->SetPlayer(Player);
			Local->SetControlRotation(Rotation);
		}
	}
	// What the server replicated here: its role here is a proxy's. The map's own actors (a net startup actor such as the
	// world settings) and the client's own stay.
	TArray<AActor*> Doomed;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor || Actor->IsNetStartupActor() || Actor->GetLocalRole() == ROLE_Authority) continue;
		Doomed.Add(Actor);
	}
	for (AActor* Actor : Doomed) World->DestroyActor(Actor, /*bNetForce*/ true);
	if (Doomed.Num() > 0) UE_LOG(LogCubeWorld, Verbose, TEXT("seamless travel: %d actor(s) of the old server destroyed"), Doomed.Num());
}

void UCubeGameEngine::DropLocalGameMode(UWorld* World)
{
	AGameModeBase* Mode = World ? World->GetAuthGameMode() : nullptr;
	if (!Mode) return;
	if (AGameStateBase* State = World->GetGameState()) { World->DestroyActor(State, true); World->SetGameState(nullptr); }
	World->DestroyActor(Mode, true);
	// UWorld keeps the game mode in a private property; a network client's world has none.
	if (FObjectPropertyBase* Property = FindFProperty<FObjectPropertyBase>(UWorld::StaticClass(), TEXT("AuthorityGameMode")))
		Property->SetObjectPropertyValue_InContainer(World, nullptr);
}
