#include "S2SBeacon.h"
#include "S2SNetDriver.h"
#include "S2STypes.h"
#include "Engine/NetConnection.h"

AS2SBeaconHost::AS2SBeaconHost(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	NetDriverDefinitionName = FName(S2S_NETDRIVER_DEFINITION);
}

AS2SBeaconClient::AS2SBeaconClient(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	NetDriverDefinitionName = FName(S2S_NETDRIVER_DEFINITION);
}

void AS2SBeaconClient::OnConnected()
{
	Super::OnConnected();
	bLinked = true;
	UE_LOG(LogS2S, Display, TEXT("[S2S %s] beacon: client beacon connected to the host (%s)"), *S2SStats::Who(),
		GetNetConnection() ? *GetNetConnection()->Describe() : TEXT("no connection"));
}

void AS2SBeaconClient::OnFailure(EBeaconFailureReason Reason, FStringView ErrorMessage)
{
	UE_LOG(LogS2S, Warning, TEXT("[S2S %s] beacon: client beacon failed (%d): %.*s"), *S2SStats::Who(), (int32)Reason, ErrorMessage.Len(), ErrorMessage.GetData());
	bLinked = false;
	AOnlineBeacon::OnFailure(Reason, ErrorMessage);
}

AS2SBeaconHostObject::AS2SBeaconHostObject(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	ClientBeaconActorClass = AS2SBeaconClient::StaticClass();
	BeaconTypeName = ClientBeaconActorClass->GetName();
}

void AS2SBeaconHostObject::OnClientConnected(AOnlineBeaconClient* NewClientActor, UNetConnection* ClientConnection)
{
	Super::OnClientConnected(NewClientActor, ClientConnection);
	UE_LOG(LogS2S, Display, TEXT("[S2S %s] beacon: peer beacon joined; view target %s (%s)"), *S2SStats::Who(),
		ClientConnection && ClientConnection->OwningActor ? *ClientConnection->OwningActor->GetName() : TEXT("NONE"),
		ClientConnection ? *ClientConnection->Describe() : TEXT("-"));
}
