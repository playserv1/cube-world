// A lab player: a stock ACharacter with its ability system on the player state (Lyra's layout). The same class is
// the ghost a neighbour server shows of it: no controller, moved to where its owner says it is, every effect applied
// to it forwarded to its owner. It shows the Burning cue as flames over its head and the Blast cue as a flash.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "AbilitySystemInterface.h"
#include "GameplayCueInterface.h"
#include "GasLabCharacter.generated.h"

class UGasLabAbilitySystemComponent;
class UProceduralMeshComponent;
class USpringArmComponent;
class UCameraComponent;

UCLASS()
class CUBEWORLD_API AGasLabCharacter : public ACharacter, public IAbilitySystemInterface, public IGameplayCueInterface
{
	GENERATED_BODY()

public:
	AGasLabCharacter();

	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;
	UGasLabAbilitySystemComponent* GetLabASC() const;
	FString GetPlayerId() const;
	bool IsGhost() const;

	virtual void PossessedBy(AController* NewController) override;
	virtual void OnRep_PlayerState() override;
	/** On the owning client once possession arrives: the ability system learns its controller (else LocalPredicted refuses). */
	virtual void PawnClientRestart() override;
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void HandleGameplayCue(AActor* Self, FGameplayTag GameplayCueTag, EGameplayCueEvent::Type EventType, const FGameplayCueParameters& Parameters) override;

	/** Hooks the ability system to this body (server on possess or ghost spawn, client when the player state arrives). */
	void InitAbilityActorInfo();
	/** A ghost glides to where its owner last said it was, a little ahead by its velocity. */
	void SetGhostTarget(const FVector& Location, const FVector& Velocity, float Yaw);

	/** Where the next throw aims (the server's bots and scenarios set it; a player throws along the view). */
	TOptional<FVector> ThrowTarget;

	UPROPERTY() TObjectPtr<USpringArmComponent> CameraBoom;
	UPROPERTY() TObjectPtr<UCameraComponent> Camera;

private:
	void BuildBody();

	UPROPERTY() TObjectPtr<UProceduralMeshComponent> Body;
	UPROPERTY() TObjectPtr<UProceduralMeshComponent> Flames;
	UPROPERTY() TObjectPtr<UProceduralMeshComponent> Flash;
	FVector GhostLocation = FVector::ZeroVector, GhostVelocity = FVector::ZeroVector;
	float GhostYaw = 0.f;
	double GhostHeard = 0, FlashUntil = 0;
	int32 BuiltFor = -2;
};
