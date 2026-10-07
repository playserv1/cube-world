// What crosses a server border, as bytes. GAS has no way to turn a spec into bytes: its handle refuses NetSerialize,
// the SetByCaller maps are not even UPROPERTYs, and captured attributes are live aggregators. So the lab writes the
// parts that decide an effect's outcome, by name, and the receiving server builds an equal spec from them:
//   - the definition (class path), level, stack count, a locked duration;
//   - SetByCaller magnitudes, dynamic tags, the source's tags as they were captured;
//   - the context: the instigator's player id, the region it was made in, the origin;
//   - every source attribute capture as {base, final} values, re-injected as an aggregator on the other side.
// Target captures are taken again on the target's server, as GAS does anyway.
#pragma once

#include "CoreMinimal.h"
#include "GameplayEffectTypes.h"

struct FGameplayEffectSpec;
class UAbilitySystemComponent;

/** A message type on the server-to-server link. */
enum class EGasLabMsg : uint8
{
	Hello = 1,
	Ghosts = 2,     // players near a border, for the neighbour to mirror
	Creeper = 3,    // a creeper the neighbour should see (and, in area mode, apply)
	Effect = 4,     // an effect applied to a ghost: the owner applies it
	Credit = 5,     // damage an instigator's effect did on another server
	Handoff = 6,    // a player (or bot) moving to the receiving server, with its whole GAS state
	HandoffAck = 7,
	Probe = 8,      // a timestamped echo, to measure the link
	ProbeEcho = 9,
	Start = 10,     // the lowest region tells everyone when the scenario starts
};

struct CUBEWORLD_API FGasLabWireSpec
{
	FString Def;
	float Level = 1.f;
	int32 StackCount = 1;
	/** Locked total duration, or -1 to let the definition decide. */
	float Duration = -1.f;
	TArray<TPair<FName, float>> SetByCallerTags;
	TArray<TPair<FName, float>> SetByCallerNames;
	TArray<FName> DynamicGrantedTags, DynamicAssetTags, SourceActorTags, SourceSpecTags;
	FString InstigatorId;
	int32 OriginRegion = -1;
	double EventAt = 0;
	bool bHasOrigin = false;
	FVector Origin = FVector::ZeroVector;
	struct FCapture
	{
		/** "UGasLabAttributeSet.BlastPower" */
		FString Attribute;
		bool bSnapshot = true;
		float Base = 0.f, Final = 0.f;
	};
	TArray<FCapture> SourceCaptures;

	/** Writes what Spec needs to be built again elsewhere. Live (non-snapshot) source captures are frozen here. */
	static FGasLabWireSpec From(const FGameplayEffectSpec& Spec);
	/** An equal spec on this server: the context names Instigator (a local ASC, or null for a remote one). */
	TSharedPtr<FGameplayEffectSpec> Build(UAbilitySystemComponent* Instigator) const;

	friend FArchive& operator<<(FArchive& Ar, FGasLabWireSpec& S);
};

/** An active effect as it stands, for a handoff. */
struct CUBEWORLD_API FGasLabWireActive
{
	FGasLabWireSpec Spec;
	/** Total duration, -1 for infinite. */
	float Duration = -1.f;
	/** Seconds since it started, on the sender's clock at SentAt. */
	float Elapsed = 0.f;
	/** When it started, on the shared clock (meaningful only with a shared clock). */
	double StartShared = 0;
	/** Seconds to the next periodic tick, -1 when not periodic. */
	float PeriodRemaining = -1.f;

	friend FArchive& operator<<(FArchive& Ar, FGasLabWireActive& A);
};

/** A whole player for a handoff: where it is and everything GAS holds about it. */
struct CUBEWORLD_API FGasLabWirePlayer
{
	FString Id, Name;
	bool bBot = false;
	int32 FromRegion = -1;
	FVector Location = FVector::ZeroVector, Velocity = FVector::ZeroVector;
	float Yaw = 0.f;
	/** Attribute base values: current values follow from them and the re-applied effects. */
	TArray<TPair<FString, float>> Bases;
	TArray<FGasLabWireActive> Effects;
	/** Shared clock when the sender captured it, and the sender's own world time then. */
	double SentAt = 0;
	/** Damage dealt and kills so far, so the credit counter carries over. */
	float DamageDealt = 0.f;
	int32 Kills = 0;

	friend FArchive& operator<<(FArchive& Ar, FGasLabWirePlayer& P);
};

/** A player near a border, for the neighbour's mirror (its ghost). */
struct CUBEWORLD_API FGasLabWireGhost
{
	FString Id, Name;
	int32 OwnerRegion = -1;
	FVector Location = FVector::ZeroVector, Velocity = FVector::ZeroVector;
	float Yaw = 0.f;
	float Health = 0.f, MaxHealth = 0.f;
	TArray<FName> Tags, Cues;
	double SentAt = 0;

	friend FArchive& operator<<(FArchive& Ar, FGasLabWireGhost& G);
};

/** A creeper the neighbour should know about. */
struct CUBEWORLD_API FGasLabWireCreeper
{
	FString Id, ThrowerId;
	int32 OwnerRegion = -1;
	FVector Origin = FVector::ZeroVector, Velocity = FVector::ZeroVector;
	/** Shared-clock launch time: everyone flies it the same from here. */
	double LaunchAt = 0;
	bool bExploded = false;
	FVector ExplodedAt = FVector::ZeroVector;
	double ExplodedAtTime = 0;
	/** The effects it carries (area mode: the neighbour applies them to its own players). */
	bool bCarriesEffects = false;
	FGasLabWireSpec Blast, Burn;
	float Radius = 0.f;

	friend FArchive& operator<<(FArchive& Ar, FGasLabWireCreeper& C);
};

struct CUBEWORLD_API FGasLabWireCredit
{
	FString InstigatorId, TargetId;
	float Damage = 0.f;
	bool bKilled = false;
	/** When the damage was done, shared clock. */
	double At = 0;

	friend FArchive& operator<<(FArchive& Ar, FGasLabWireCredit& C);
};

namespace GasLabWire
{
	/** "UGasLabAttributeSet.Health" for an attribute, and back. */
	FString AttributeName(const FGameplayAttribute& Attribute);
	FGameplayAttribute FindAttribute(const FString& Name);
	TArray<FName> TagNames(const FGameplayTagContainer& Tags);
	FGameplayTagContainer Tags(const TArray<FName>& Names);
}
