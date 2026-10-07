// The player state holds the ability system, as Lyra's does. A ghost (the mirror of a player another server owns)
// gets one too, spawned by the border layer, so this server's clients see the ghost's health, tags and cues through
// GAS's own replication.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerState.h"
#include "AbilitySystemInterface.h"
#include "GasLabPlayerState.generated.h"

class UGasLabAbilitySystemComponent;
class UGasLabAttributeSet;

UCLASS()
class CUBEWORLD_API AGasLabPlayerState : public APlayerState, public IAbilitySystemInterface
{
	GENERATED_BODY()

public:
	AGasLabPlayerState();

	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;
	UGasLabAbilitySystemComponent* GetLabASC() const { return AbilitySystem; }
	const UGasLabAttributeSet* GetAttributes() const { return Attributes; }

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** The player's id, the same on every server. */
	UPROPERTY(Replicated) FString LabId;
	/** A mirror of a player another server owns. */
	UPROPERTY(Replicated) bool bGhost = false;
	/** The region of the server that owns this player. */
	UPROPERTY(Replicated) int32 OwnerRegion = -1;

private:
	UPROPERTY(VisibleAnywhere) TObjectPtr<UGasLabAbilitySystemComponent> AbilitySystem;
	UPROPERTY() TObjectPtr<UGasLabAttributeSet> Attributes;
};
