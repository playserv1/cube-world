#include "CubeWorldState.h"
#include "CubeWorld.h"
#include "CubeWorldGameInstance.h"
#include "Net/UnrealNetwork.h"

ACubeWorldState::ACubeWorldState()
{
	bReplicates = true;
	bAlwaysRelevant = true;
	bNetLoadOnClient = false;
	bReplicateUsingRegisteredSubObjectList = true;
	SetNetUpdateFrequency(20.f);
	PrimaryActorTick.bCanEverTick = false;
}

void ACubeWorldState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ACubeWorldState, Server);
	DOREPLIFETIME(ACubeWorldState, Color);
	DOREPLIFETIME(ACubeWorldState, Region);
	DOREPLIFETIME(ACubeWorldState, Regions);
	DOREPLIFETIME(ACubeWorldState, Players);
}

void ACubeWorldState::BeginPlay()
{
	Super::BeginPlay();
	// A client that got the actor with its first state already filled in sees no OnRep for it.
	if (GetNetMode() == NM_Client) { OnRep_Regions(); OnRep_Players(); }
}

UCubeWorldGameInstance* ACubeWorldState::Game() const
{
	return GetNetMode() == NM_DedicatedServer || CubeIsServerProcess() ? nullptr : Cast<UCubeWorldGameInstance>(GetGameInstance());
}

void ACubeWorldState::OnRep_Info() {}

void ACubeWorldState::OnRep_Regions()
{
	if (UCubeWorldGameInstance* G = Game()) G->SetRegions(Regions);
}

void ACubeWorldState::OnRep_Players()
{
	if (UCubeWorldGameInstance* G = Game()) G->SetPlayers(Players);
}

void ACubeWorldState::MulticastCubes_Implementation(const TArray<FCubeChangeRep>& Changes, const TArray<FCubeFallRep>& Falls, bool bRemote)
{
	if (UCubeWorldGameInstance* G = Game()) G->ApplyCubes(Changes, Falls, bRemote);
}

void ACubeWorldState::MulticastDig_Implementation(const FString& PlayerId, int32 X, int32 Y, int32 Z, int32 Stage)
{
	if (UCubeWorldGameInstance* G = Game()) G->OnDig.Broadcast(PlayerId, X, Y, Z, Stage);
}

void ACubeWorldState::MulticastHurt_Implementation(const FString& PlayerId, float Health, float KX, float KY, float Strength, const FString& By)
{
	if (UCubeWorldGameInstance* G = Game()) G->OnHurtFrame(PlayerId, Health, KX, KY, Strength);
}

void ACubeWorldState::MulticastDeath_Implementation(const FString& PlayerId, const FString& By)
{
	if (UCubeWorldGameInstance* G = Game()) G->OnDeathFrame(PlayerId, By);
}

void ACubeWorldState::MulticastBomb_Implementation(const FCubeBombRep& Bomb)
{
	if (UCubeWorldGameInstance* G = Game()) G->OnBombFrame(Bomb);
}
