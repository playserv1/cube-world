#include "S2SNetDriver.h"
#include "S2STypes.h"
#include "Engine/Level.h"
#include "Engine/NetConnection.h"
#include "Engine/World.h"
#include "Misc/NetworkVersion.h"
#include "Net/DataChannel.h"
#include "GameFramework/Actor.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

US2SNetDriver::US2SNetDriver(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

bool US2SNetDriver::IsMirrored(const AActor* Actor)
{
	const IS2SMirrored* Mirrored = Cast<const IS2SMirrored>(Actor);
	return Mirrored && Mirrored->ShouldMirrorToPeers();
}

bool US2SNetDriver::ShouldReplicateActor(AActor* Actor) const
{
	// The driver's own actors (a beacon's client actor is named after its driver) and, in server mode, the opted-in
	// authoritative game actors.
	if (Super::ShouldReplicateActor(Actor)) return true;
	return IsServer() && Actor && Actor->GetLocalRole() == ROLE_Authority && IsMirrored(Actor);
}

bool US2SNetDriver::ShouldReplicateFunction(AActor* Actor, UFunction* Function) const
{
	if (Super::ShouldReplicateFunction(Actor, Function)) return true;
	return IsServer() && Actor && Actor->GetLocalRole() == ROLE_Authority && IsMirrored(Actor);
}

// ---------------------------------------------------------------------------------------------------------------------

EAcceptConnection::Type FS2SHostNotify::NotifyAcceptingConnection()
{
	return EAcceptConnection::Accept;
}

void FS2SHostNotify::NotifyAcceptedConnection(UNetConnection* Connection)
{
	UE_LOG(LogS2S, Display, TEXT("[S2S %s] host: peer connection accepted: %s"), *S2SStats::Who(), *Connection->LowLevelGetRemoteAddress(true));
}

bool FS2SHostNotify::NotifyAcceptingChannel(UChannel* Channel)
{
	// As UWorld::NotifyAcceptingChannel on a server: the channels a client may open (control, voice, Iris's data stream).
	const bool bAccept = Channel->Connection->Driver->ChannelDefinitionMap[Channel->ChName].bClientOpen;
	UE_LOG(LogS2S, Log, TEXT("[S2S %s] host: peer opens channel %s: %s"), *S2SStats::Who(), *Channel->ChName.ToString(), bAccept ? TEXT("accepted") : TEXT("refused"));
	return bAccept;
}

void FS2SHostNotify::NotifyControlMessage(UNetConnection* Connection, uint8 MessageType, FInBunch& Bunch)
{
	UWorld* InWorld = World.Get();
	switch (MessageType)
	{
	case NMT_Hello:
	{
		uint8 IsLittleEndian = 0;
		uint32 RemoteNetworkVersion = 0;
		FString EncryptionToken;
		EEngineNetworkRuntimeFeatures RemoteFeatures = EEngineNetworkRuntimeFeatures::None;
		if (!FNetControlMessage<NMT_Hello>::Receive(Bunch, IsLittleEndian, RemoteNetworkVersion, EncryptionToken, RemoteFeatures)) break;

		uint32 LocalNetworkVersion = FNetworkVersion::GetLocalNetworkVersion();
		EEngineNetworkRuntimeFeatures LocalFeatures = Connection->Driver->GetNetworkRuntimeFeatures();
		const bool bCompatible = FNetworkVersion::IsNetworkCompatible(LocalNetworkVersion, RemoteNetworkVersion)
			&& FNetworkVersion::AreNetworkRuntimeFeaturesCompatible(LocalFeatures, RemoteFeatures);
		UE_LOG(LogS2S, Display, TEXT("[S2S %s] host: Hello from peer (version %u vs ours %u, iris %d vs ours %d)"), *S2SStats::Who(),
			RemoteNetworkVersion, LocalNetworkVersion,
			EnumHasAnyFlags(RemoteFeatures, EEngineNetworkRuntimeFeatures::IrisEnabled) ? 1 : 0,
			EnumHasAnyFlags(LocalFeatures, EEngineNetworkRuntimeFeatures::IrisEnabled) ? 1 : 0);
		if (!bCompatible)
		{
			FNetControlMessage<NMT_Upgrade>::Send(Connection, LocalNetworkVersion, LocalFeatures);
			Connection->FlushNet(true);
			Connection->Close(ENetCloseResult::Upgrade);
			break;
		}
		// A production link would check a shared secret here (the Login URL carries options); the spike admits any peer.
		Connection->SendChallengeControlMessage();
		break;
	}
	case NMT_Netspeed:
	{
		int32 Rate = 0;
		if (FNetControlMessage<NMT_Netspeed>::Receive(Bunch, Rate))
		{
			Connection->CurrentNetSpeed = FMath::Clamp(Rate, 1800, Connection->Driver->MaxClientRate);
			UE_LOG(LogS2S, Display, TEXT("[S2S %s] host: peer netspeed %d (asked %d)"), *S2SStats::Who(), Connection->CurrentNetSpeed, Rate);
		}
		break;
	}
	case NMT_Login:
	{
		FUniqueNetIdRepl UniqueId;
		FString OnlinePlatformName;
		if (!FNetControlMessage<NMT_Login>::Receive(Bunch, Connection->ClientResponse, Connection->RequestURL, UniqueId, OnlinePlatformName)) break;
		if (!InWorld || !InWorld->PersistentLevel) { Connection->Close(); break; }

		// Iris replicates a level's actors only to connections whose world is ours (UNetDriver::UpdateGroupFilterStatusFor
		// AllLevelsForConnection): the peer runs the same map.
		UPackage* WorldPackage = InWorld->PersistentLevel->GetOutermost();
		Connection->SetClientWorldPackageName(WorldPackage->GetFName());
		FString LevelName = WorldPackage->GetName();
		FString GameName;
		FString RedirectURL;
		FNetControlMessage<NMT_Welcome>::Send(Connection, LevelName, GameName, RedirectURL);
		Connection->FlushNet();
		Connection->SetClientLoginState(EClientLoginState::Welcomed);
		UE_LOG(LogS2S, Display, TEXT("[S2S %s] host: Login from peer %s, welcomed to %s"), *S2SStats::Who(), *Connection->RequestURL, *LevelName);
		break;
	}
	case NMT_Join:
	case NMT_JoinNoPawn:
	{
		// No PlayerController, no pawn: ReceivedJoin is what turns Iris replication on for the connection
		// (UNetConnection::SetClientLoginState -> SetReplicationEnabledForConnection).
		Connection->SetClientLoginState(EClientLoginState::ReceivedJoin);

		// Iris prioritises nothing for a connection without a view (FReplicationPrioritization::Prioritize skips it), so
		// after the initial state no update would ever be scheduled again. With no PlayerController the connection's view
		// target is its OwningActor (UNetConnection::GetConnectionViewTarget): a bare actor at the border, which is also
		// where a spatial filter would centre the peer's view.
		static const bool bNoView = FParse::Param(FCommandLine::Get(), TEXT("s2snoview"));
		if (!bNoView && InWorld)
		{
			FActorSpawnParameters Params;
			Params.Name = MakeUniqueObjectName(InWorld->PersistentLevel, AActor::StaticClass(), TEXT("S2SPeerViewpoint"));
			AActor* Viewpoint = InWorld->SpawnActor<AActor>(AActor::StaticClass(), FTransform(FVector::ZeroVector), Params);
			Connection->OwningActor = Viewpoint;
		}
		UE_LOG(LogS2S, Display, TEXT("[S2S %s] host: peer joined; replication to it is on, view target %s (%s)"), *S2SStats::Who(),
			Connection->OwningActor ? *Connection->OwningActor->GetName() : TEXT("NONE"), *Connection->Describe());
		break;
	}
	default:
		UE_LOG(LogS2S, Log, TEXT("[S2S %s] host: control message %s from peer ignored"), *S2SStats::Who(),
			FNetControlMessageInfo::IsRegistered(MessageType) ? FNetControlMessageInfo::GetName(MessageType) : TEXT("?"));
		break;
	}
}

// ---------------------------------------------------------------------------------------------------------------------

void US2SPeerLink::NotifyControlMessage(UNetConnection* Connection, uint8 MessageType, FInBunch& Bunch)
{
	Super::NotifyControlMessage(Connection, MessageType, Bunch);

	if (MessageType == NMT_Welcome && !bJoined)
	{
		// UPendingNetGame would load the map first and join from TravelCompleted; the peer server has its world already.
		SendJoin();
		// The stock client opens its connection when the server's PlayerController arrives (HandleClientPlayer). A peer
		// gets none.
		Connection->SetConnectionState(USOCK_Open);
		bJoined = true;
		JoinedAt = FPlatformTime::Seconds();
		UE_LOG(LogS2S, Display, TEXT("[S2S %s] peer: welcomed by host (%s), joined"), *S2SStats::Who(), *Connection->Describe());
	}
	else if (MessageType == NMT_Failure || MessageType == NMT_Upgrade)
	{
		UE_LOG(LogS2S, Warning, TEXT("[S2S %s] peer: host refused: %s"), *S2SStats::Who(), *ConnectionError);
	}
}
