// Runs the server-to-server spike in a world. Created only when the process was started with -s2sspike.
//
// Switches (servers are started with ?game=/Script/CubeWorld.S2SSpikeGameMode so no Cube World logic runs):
//   -s2sname=A                 the name this process logs under
//   -s2shost=<port>            server A: listen for peers on an S2S driver at <port>; spawn the mirrored test actors
//   -s2sprojectiles=<n>        server A: number of projectiles (default 1)
//   -s2sascmode=Mixed|Full|Minimal   server A: the ASC actor's replication mode (default Mixed)
//   -s2speer=<host:port>       server B: connect an S2S driver to A; spawn B's own target and control actors
//   -s2srelay=anyrole[,swap][,frontend] | localswap   server B: the engine's relay switches on its game driver (and the S2S driver
//                              as the Backend for frontend): EnableReplicationOfActorsWithAnyRole, EnableRoleSwappingOf
//                              AllActors, SetProxyType(Frontend/Backend)
//   -s2sbeaconhost=<port>, -s2sbeaconpeer=<host:port>   the same link over OnlineBeacons (S2SBeacon.h)
//   -s2squitafter=<seconds>    log the cumulative numbers and exit
// Clients (to A's or B's game port) take -s2sspike too, and log what they see.
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "GameplayTagContainer.h"
#include "GameplayEffectTypes.h"
#include "GameplayCueInterface.h"
#include "GameplayCueManager.h"
#include "S2SSpikeSubsystem.generated.h"

class UNetDriver;
class UNetConnection;
class US2SPeerLink;
class FS2SHostNotify;
class AS2SAscActor;
class AS2SProjectile;

UCLASS()
class US2SSpikeSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	US2SSpikeSubsystem();
	virtual ~US2SSpikeSubsystem() override;

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	void HandleNetDriverCreated(UWorld* InWorld, UNetDriver* Driver);
	void StartHost(int32 Port);
	void StartPeer(const FString& Address);
	void StartBeaconHost(int32 Port);
	void StartBeaconPeer(const FString& Address);
	void SpawnHostActors();
	void SpawnPeerActors();

	void LogTraffic(bool bCumulative);
	void LogConnection(const FString& Label, UNetConnection* Connection, bool bCumulative);
	void LogInventory();
	void LogCues();
	void RunGasChecks(AS2SAscActor* Copy);
	void CheckBorderHits();
	void Finish();

	void HandleCueRouted(AActor* Target, FGameplayTag Tag, EGameplayCueEvent::Type Event, const FGameplayCueParameters& Params, EGameplayCueExecutionOptions Options);

	/** The S2S driver: A's server-mode one, or B's client-mode one. */
	UPROPERTY()
	TObjectPtr<UNetDriver> S2SDriver;

	UPROPERTY()
	TObjectPtr<US2SPeerLink> PeerLink;

	UPROPERTY()
	TObjectPtr<AS2SAscActor> LocalTarget;

	UPROPERTY()
	TObjectPtr<AActor> BeaconHost;

	UPROPERTY()
	TObjectPtr<AActor> BeaconHostObject;

	UPROPERTY()
	TObjectPtr<AActor> BeaconClient;

	bool bBeaconPeer = false;

	TUniquePtr<FS2SHostNotify> HostNotify;
	FName S2SDriverName;

	bool bServer = false;
	bool bRelayAnyRole = false;
	bool bRelaySwap = false;
	bool bRelayFrontend = false;
	/** No engine switch: B flips each copy's roles itself and starts replicating it on its game driver. */
	bool bRelayLocalSwap = false;
	void LocalSwapCopies();
	FString PeerAddress;
	double StartedAt = 0.0;
	double NextReportAt = 0.0;
	double QuitAt = 0.0;
	double PeerConnectAt = 0.0;
	bool bFinished = false;

	/** B: when an ASC copy was first seen, and whether the GAS checks ran on it. */
	double CopySeenAt = 0.0;
	bool bGasChecked = false;
	double GasCheckedAt = 0.0;
	TArray<FString> GasResults;

	/** B: each copied projectile's last x, to spot the frame it crosses into B's half (x >= 0). */
	TMap<TWeakObjectPtr<AS2SProjectile>, double> LastX;
	int32 BorderHits = 0;

	struct FTrafficSample
	{
		int64 InBytes = 0, OutBytes = 0;
		int64 InPackets = 0, OutPackets = 0;
		double At = 0.0;
	};
	TMap<TWeakObjectPtr<UNetConnection>, FTrafficSample> LastTraffic;
	TMap<TWeakObjectPtr<UNetConnection>, FTrafficSample> FirstTraffic;

	/** "tag event role origin" -> count, from the GameplayCue manager's route hook. */
	TMap<FString, int32> CueRoutes;
	FDelegateHandle CueRouteHandle;
	FDelegateHandle NetDriverCreatedHandle;
};
