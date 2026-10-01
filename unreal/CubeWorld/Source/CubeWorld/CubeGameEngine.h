// The client's engine. It moves the player from one server to the next without unloading the world.
//
// Unreal's own client travel is break-before-make: Browse shuts the world's connection down before the next server is
// even asked, and LoadMap tears the whole world down once it answers: the meshes, the other players, the bombs, the
// view. Every Cube World server plays the same map and the client already holds everything it draws, so here the
// travel is make-before-break and keeps the world:
//   1. the old connection plays on while the next server's handshake runs (ShouldShutdownWorldNetDriver);
//   2. when the next server welcomes the client, its connection takes the old one's place in the same world
//      (LoadMap -> AdoptPendingNetGame): the old server's actors go, the client's own stay, and the engine then
//      joins the next server as after any LoadMap;
//   3. a handshake that fails, or does not let the client in within ten seconds, is given up and leaves the player on
//      the old server, still connected (the stock engine would disconnect from it and load the default map).
// Leaving an Unreal server for a C# one (a socket from the client's own world) keeps the world the same way
// (DisconnectKeepWorld). The C# side needs nothing: a socket can be opened next to the old one, as the browser does.
#pragma once

#include "CoreMinimal.h"
#include "Engine/GameEngine.h"
#include "CubeGameEngine.generated.h"

DECLARE_MULTICAST_DELEGATE_TwoParams(FCubeOnServerSwitched, UWorld* /*World*/, bool /*bConnected: a next server's connection took over; false: no server*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FCubeOnSeamlessTravelFailed, const FString& /*Why*/);

UCLASS()
class CUBEWORLD_API UCubeGameEngine : public UGameEngine
{
	GENERATED_BODY()

public:
	/** The engine the client runs on, or null (the editor's, a server's own run of the stock one). */
	static UCubeGameEngine* Get();

	/**
	 * The next ClientTravel of this world keeps it: the old server plays on until the next one welcomes the client, then
	 * the next one's connection takes over in the same world. Cleared when that travel is done or has failed.
	 */
	void RequestSeamlessTravel();
	bool IsSeamlessTravelPending() const { return bSeamlessRequested; }

	/**
	 * Leaves the server the world is connected to and keeps the world as the client's own: the server's actors go and
	 * the local player gets a local controller. For a C# server, reached from here over a socket.
	 */
	void DisconnectKeepWorld(UWorld* World);

	/** Fired once the world changed hands: a next server's connection took over (true), or the world has no server now (false). */
	FCubeOnServerSwitched OnServerSwitched;
	/** Fired when a seamless travel was given up: the next server refused or never answered. The old connection plays on. */
	FCubeOnSeamlessTravelFailed OnSeamlessTravelFailed;

	virtual void Tick(float DeltaSeconds, bool bIdleMode) override;
	virtual bool ShouldShutdownWorldNetDriver() override;
	virtual bool LoadMap(FWorldContext& WorldContext, FURL URL, class UPendingNetGame* Pending, FString& Error) override;
	virtual void HandleNetworkFailure(UWorld* World, UNetDriver* NetDriver, ENetworkFailure::Type FailureType, const FString& ErrorString) override;

private:
	bool AdoptPendingNetGame(FWorldContext& Context, const FURL& URL, UPendingNetGame* Pending, FString& Error);
	/** Destroys what the server replicated into this world and gives the local player a local controller in place of the server's. */
	void ClearServerActors(UWorld* World);
	/** A world loaded as the client's own (the menu map) has a game mode and a game state of its own; a network client has neither. */
	void DropLocalGameMode(UWorld* World);

	bool bSeamlessRequested = false;
	double SeamlessStartedAt = 0;
	/** Why the handshake failed, heard while its own driver ticked; the travel is given up on the engine's next tick. */
	FString SeamlessFailure;
	/** The stock engine waits 20 s (InitialConnectTimeout) for a server that never answers: long to stand at a border. */
	static constexpr double SeamlessTimeoutSeconds = 10.0;
};
