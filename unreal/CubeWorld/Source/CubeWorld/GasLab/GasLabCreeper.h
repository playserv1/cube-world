// The creeper: Cube World's creeper head, thrown by an ability and carrying its two effects as GAS projectiles do
// (the ability makes the specs, the projectile applies them where it blows). Its flight is a function of its launch,
// stamped on the shared clock, so every server and client flies it the same without streaming it.
//
// The thrower's server owns it and blows it up. A neighbour gets a ghost of it from the border layer: the same class,
// replicated to the neighbour's own clients, flown the same way, exploding when the owner says it did. How the blast
// reaches the neighbour's players is the lab's switch (-gasmode=):
//   forward  the owner applies the effects to everyone in the radius it can see, ghosts included; a ghost's ability
//            system forwards the spec to the server that owns the player (the default);
//   area     the owner applies them to its own players only; the ghost creeper carries the effects to the neighbour,
//            which applies them to its own players in the radius.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GameplayEffectTypes.h"
#include "GasLab/GasLabWire.h"
#include "GasLabCreeper.generated.h"

class ACubeBomb;
class UGasLabAbilitySystemComponent;

UCLASS()
class CUBEWORLD_API AGasLabCreeper : public AActor
{
	GENERATED_BODY()

public:
	AGasLabCreeper();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;

	/** Where the flight has it at shared time T. */
	FVector PositionAt(double T) const;

	static constexpr float Gravity = -980.f;
	static constexpr float Fuse = 2.2f;
	static constexpr float Radius = 650.f;
	static constexpr float GroundZ = 20.f;

	UPROPERTY(Replicated) FString Id;
	UPROPERTY(Replicated) FString ThrowerId;
	UPROPERTY(Replicated) int32 OwnerRegion = -1;
	UPROPERTY(Replicated) FVector_NetQuantize10 Origin;
	UPROPERTY(Replicated) FVector_NetQuantize10 Velocity;
	UPROPERTY(Replicated) double LaunchAt = 0;
	UPROPERTY(ReplicatedUsing = OnRep_Exploded) bool bExploded = false;
	UPROPERTY(Replicated) FVector_NetQuantize10 ExplodedAt;
	/** A neighbour's mirror of a creeper another server owns. */
	UPROPERTY(Replicated) bool bGhost = false;

	/** Owner only: the specs the ability made. */
	FGameplayEffectSpecHandle BlastSpec, BurnSpec;
	/** Ghost only, area mode: the specs as they came over the border. */
	TOptional<FGasLabWireSpec> GhostBlast, GhostBurn;

	/** The owner blows it up: effects to whoever the mode says, the explosion to the neighbours. */
	void Explode();
	/** A ghost hears that its owner blew it up at At. */
	void ExplodedElsewhere(const FVector& At, double When);

private:
	UFUNCTION() void OnRep_Exploded();
	/** Applies Spec to the ability systems around the centre; bOwnOnly leaves out ghosts. Returns how many. */
	int32 ApplyAround(const FVector& Center, const FGameplayEffectSpec& Spec, bool bOwnOnly, const TCHAR* Via);
	void Boom();

	UPROPERTY() TObjectPtr<ACubeBomb> Look;
	double ExplodedLocalAt = 0;
};
