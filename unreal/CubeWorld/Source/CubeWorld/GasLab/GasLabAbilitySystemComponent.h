// The lab's ability system component: the stock one, plus what a server border needs, and nothing a game's abilities
// or effects have to know about.
//   - A ghost is the mirror of a player another server owns. Game code applies effects to it like to anyone (the
//     creeper's blast takes every ability system in its radius); the ghost applies nothing itself and hands the spec
//     to the border layer, which sends it to the owning server, where the real player takes it.
//   - Damage an effect does is credited to its instigator, on whichever server the instigator lives.
//   - A handoff writes the whole GAS state of a player (attribute bases, active effects with their time, stacks and
//     period phase) and the next server restores it. GAS has no API for the period phase; the subclass reaches the
//     protected container and re-arms the timer itself.
#pragma once

#include "CoreMinimal.h"
#include "AbilitySystemComponent.h"
#include "GasLab/GasLabWire.h"
#include "GasLabAbilitySystemComponent.generated.h"

/** Damage a player's effects did: to whom, how much, whether it killed, and whether it was done on another server. */
DECLARE_MULTICAST_DELEGATE_FourParams(FGasLabOnDamageDealt, const FString& /*TargetId*/, float /*Damage*/, bool /*bKilled*/, bool /*bElsewhere*/);

/** How a restore went, for the lab's measurements. */
struct FGasLabRestoreReport
{
	int32 Effects = 0, Restored = 0;
	/** Per restored effect: the end it should have (shared clock) against the end it got, seconds. */
	TArray<float> EndErrors;
	/** Per periodic effect: the next tick it should have against the one it got, seconds. */
	TArray<float> PhaseErrors;
	/** Shared-clock seconds between capture on the old server and restore here. */
	float Transit = 0.f;
};

UCLASS()
class CUBEWORLD_API UGasLabAbilitySystemComponent : public UAbilitySystemComponent
{
	GENERATED_BODY()

public:
	UGasLabAbilitySystemComponent();

	/** The player's id, the same on every server. */
	FString PlayerId;
	/** A mirror of a player another server owns. */
	bool bGhost = false;
	/** The region of the server that owns a ghost. */
	int32 OwnerRegion = -1;
	/** Set while the player is being handed to another server: effects that arrive meanwhile go after them. */
	bool bLeaving = false;

	virtual FActiveGameplayEffectHandle ApplyGameplayEffectSpecToSelf(const FGameplayEffectSpec& GameplayEffect, FPredictionKey PredictionKey = FPredictionKey()) override;
	virtual FGameplayEffectContextHandle MakeEffectContext() const override;

	/** Applies a spec here whatever this component is (the receiving end of a forward or a restore). */
	FActiveGameplayEffectHandle ApplyHere(const FGameplayEffectSpec& Spec);

	/** Health lost here to Spec; credits its instigator here or on its server. */
	void NoteDamage(const FGameplayEffectSpec& Spec, float Lost, bool bKilled);
	/** Credit for damage this player's effects did (bElsewhere: on another server, heard over the border). */
	void Credit(const FString& TargetId, float Damage, bool bKilled, bool bElsewhere);

	/** The whole GAS state for a handoff: attribute bases and the active effects as they stand now. */
	void CaptureState(FGasLabWirePlayer& Out) const;
	/** Rebuilds a state captured on another server: bases first, then every effect with its time and period phase. */
	void RestoreState(const FGasLabWirePlayer& State, FGasLabRestoreReport& Report);

	/** A ghost takes the latest of what its owner shows: health, tags, looping cues. */
	void Mirror(const FGasLabWireGhost& Ghost);
	/** Looping cues active on this player (from effects and added by hand), for a ghost elsewhere. */
	TArray<FName> ActiveCueNames() const;

	/** Seconds left of the first active effect with Tag among its asset or granted tags, -1 if none. */
	float TimeLeft(const FGameplayTag& Tag) const;
	/** Stacks of the first active effect with Tag, 0 if none. */
	int32 Stacks(const FGameplayTag& Tag) const;
	/** Seconds to the next periodic tick of the first active effect with Tag, -1 if none. */
	float NextTickIn(const FGameplayTag& Tag) const;

	FGasLabOnDamageDealt OnDamageDealt;
	float DamageDealt = 0.f;
	int32 Kills = 0;

private:
	const FActiveGameplayEffect* FindActive(const FGameplayTag& Tag) const;

	bool bApplyingHere = false;
	TSet<FName> MirroredTags, MirroredCues;
};
