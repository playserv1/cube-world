// The same server-to-server link built from OnlineBeacons instead of a bare driver with a hand-written handshake
// (-s2sbeaconhost=<port> on A, -s2sbeaconpeer=<host:port> on B).
//
// The beacon handshake (Hello -> BeaconWelcome -> BeaconJoin -> BeaconAssignGUID -> NetGUIDAck) admits the peer without
// a PlayerController and makes the client beacon actor the connection's OwningActor, which is also the view Iris needs.
// Both ends create their driver from the spike's own definition (S2SNetDriver: US2SNetDriver, Iris on) instead of
// BeaconNetDriver (stock IpNetDriver, generic replication), so A's opted-in game actors go over the beacon link too.
#pragma once

#include "CoreMinimal.h"
#include "OnlineBeaconClient.h"
#include "OnlineBeaconHost.h"
#include "OnlineBeaconHostObject.h"
#include "S2SBeacon.generated.h"

UCLASS(transient, notplaceable)
class AS2SBeaconHost : public AOnlineBeaconHost
{
	GENERATED_BODY()

public:
	AS2SBeaconHost(const FObjectInitializer& ObjectInitializer);
};

UCLASS(transient, notplaceable)
class AS2SBeaconClient : public AOnlineBeaconClient
{
	GENERATED_BODY()

public:
	AS2SBeaconClient(const FObjectInitializer& ObjectInitializer);

	virtual void OnConnected() override;
	virtual void OnFailure(EBeaconFailureReason Reason, FStringView ErrorMessage) override;

	bool IsLinked() const { return bLinked; }

private:
	bool bLinked = false;
};

UCLASS(transient, notplaceable)
class AS2SBeaconHostObject : public AOnlineBeaconHostObject
{
	GENERATED_BODY()

public:
	AS2SBeaconHostObject(const FObjectInitializer& ObjectInitializer);

	virtual void OnClientConnected(AOnlineBeaconClient* NewClientActor, UNetConnection* ClientConnection) override;
};
