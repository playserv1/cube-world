#include "GasLab/GasLabPlayerState.h"
#include "GasLab/GasLabAbilitySystemComponent.h"
#include "GasLab/GasLabAttributeSet.h"
#include "Net/UnrealNetwork.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GasLabPlayerState)

AGasLabPlayerState::AGasLabPlayerState()
{
	AbilitySystem = CreateDefaultSubobject<UGasLabAbilitySystemComponent>(TEXT("AbilitySystem"));
	Attributes = CreateDefaultSubobject<UGasLabAttributeSet>(TEXT("Attributes"));
	// Lyra's rate for a player state that carries the ability system.
	SetNetUpdateFrequency(100.f);
}

UAbilitySystemComponent* AGasLabPlayerState::GetAbilitySystemComponent() const
{
	return AbilitySystem;
}

void AGasLabPlayerState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AGasLabPlayerState, LabId);
	DOREPLIFETIME(AGasLabPlayerState, bGhost);
	DOREPLIFETIME(AGasLabPlayerState, OwnerRegion);
}
