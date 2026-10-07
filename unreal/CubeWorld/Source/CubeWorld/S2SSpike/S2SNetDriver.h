// The second net driver between servers, and the two ends of its handshake.
//
//   server A: US2SNetDriver in server mode, listening on its own port next to A's game driver. It replicates only the
//             actors that opt in (IS2SMirrored); the same authoritative actor goes on replicating to A's own clients
//             through the game driver. Its handshake (FS2SHostNotify) admits a peer server without a PlayerController,
//             a pawn or A's game mode: Hello -> Challenge -> Login -> Welcome -> Join, then Iris replication is enabled
//             on the connection.
//   server B: US2SNetDriver in client mode in B's own world, connected to A's S2S port through US2SPeerLink (the
//             engine's UPendingNetGame, which sends the Join as soon as it is welcomed instead of loading a map). What
//             A replicates is spawned in B's world as SimulatedProxy copies.
#pragma once

#include "CoreMinimal.h"
#include "IpNetDriver.h"
#include "Engine/PendingNetGame.h"
#include "S2SNetDriver.generated.h"

/** The definition the S2S drivers are created from (registered at runtime by the spike subsystem). */
#define S2S_NETDRIVER_DEFINITION TEXT("S2SNetDriver")

UCLASS(transient, config = Engine)
class US2SNetDriver : public UIpNetDriver
{
	GENERATED_BODY()

public:
	US2SNetDriver(const FObjectInitializer& ObjectInitializer);

	/** The driver's own actors (by NetDriverName), plus in server mode the authoritative actors that opt in (IS2SMirrored). */
	virtual bool ShouldReplicateActor(AActor* Actor) const override;
	/** Server mode: multicasts of mirrored actors go to the peers too (the GameplayCue multicasts of a mirrored ASC). */
	virtual bool ShouldReplicateFunction(AActor* Actor, UFunction* Function) const override;

	static bool IsMirrored(const AActor* Actor);
};

/**
 * Server A's side of the S2S handshake. A plain FNetworkNotify: the world's own (UWorld::NotifyControlMessage) would
 * run the game mode's PreLogin/Login and spawn a PlayerController for the peer.
 */
class FS2SHostNotify : public FNetworkNotify
{
public:
	explicit FS2SHostNotify(UWorld* InWorld) : World(InWorld) {}

	virtual EAcceptConnection::Type NotifyAcceptingConnection() override;
	virtual void NotifyAcceptedConnection(UNetConnection* Connection) override;
	virtual bool NotifyAcceptingChannel(UChannel* Channel) override;
	virtual void NotifyControlMessage(UNetConnection* Connection, uint8 MessageType, FInBunch& Bunch) override;

private:
	TWeakObjectPtr<UWorld> World;
};

/** Server B's side: the stock client handshake, with the Join sent on Welcome and no map load, no local player. */
UCLASS(transient)
class US2SPeerLink : public UPendingNetGame
{
	GENERATED_BODY()

public:
	virtual EAcceptConnection::Type NotifyAcceptingConnection() override { return EAcceptConnection::Reject; }
	virtual void NotifyAcceptedConnection(UNetConnection* Connection) override {}
	virtual bool NotifyAcceptingChannel(UChannel* Channel) override { return true; }
	virtual void NotifyControlMessage(UNetConnection* Connection, uint8 MessageType, FInBunch& Bunch) override;
	virtual ULocalPlayer* GetFirstGamePlayer() override { return nullptr; }

	bool IsJoined() const { return bJoined; }
	double GetJoinedAt() const { return JoinedAt; }

private:
	bool bJoined = false;
	double JoinedAt = 0.0;
};
