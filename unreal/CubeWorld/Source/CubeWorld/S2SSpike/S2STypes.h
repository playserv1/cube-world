// Server-to-server replication spike: the pieces every other S2S file shares.
//
// The spike asks whether a second NetDriver between two dedicated servers can carry selected authoritative actors from
// server A into server B's world as ordinary replicated copies (a projectile carrying a gameplay effect, an actor with an
// ability system component), and what B can and cannot do with them. Nothing here runs unless the process was started
// with -s2sspike (see S2SSpikeSubsystem.h for the switches and Scripts/RunS2SSpike.ps1 for a run).
#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "NativeGameplayTags.h"
#include "UObject/Interface.h"
#include "UObject/SoftObjectPath.h"
#include "S2STypes.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogS2S, Log, All);

UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_S2S_SetByCaller_Damage);
UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_S2S_State_Burning);
UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_S2S_Cue_Loop);
UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_S2S_Cue_Burst);
UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_S2S_Cue_FromB);
UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_S2S_Cue_FromBLocal);

/** An actor that may be mirrored to the neighbour servers over the S2S driver (US2SNetDriver::ShouldReplicateActor). */
UINTERFACE(MinimalAPI, meta = (CannotImplementInterfaceInBlueprint))
class US2SMirrored : public UInterface
{
	GENERATED_BODY()
};

class IS2SMirrored
{
	GENERATED_BODY()

public:
	/** Asked once, when the actor starts replicating (BeginPlay, or when an S2S driver comes up after it). */
	virtual bool ShouldMirrorToPeers() const = 0;
};

/** One SetByCaller magnitude of a gameplay effect, by tag. */
USTRUCT()
struct FS2SSetByCaller
{
	GENERATED_BODY()

	UPROPERTY()
	FGameplayTag Tag;

	UPROPERTY()
	float Magnitude = 0.f;
};

/**
 * What a projectile carries instead of a FGameplayEffectSpec, which cannot be replicated (its handle's NetSerialize is
 * fatal, its SetByCaller maps are no UPROPERTY): enough for the receiving server to rebuild the spec and apply it.
 */
USTRUCT()
struct FS2SEffectPayload
{
	GENERATED_BODY()

	/** The gameplay effect class, by path, so the other server loads the same class. */
	UPROPERTY()
	FSoftClassPath EffectClass;

	UPROPERTY()
	float Level = 1.f;

	UPROPERTY()
	TArray<FS2SSetByCaller> SetByCallers;

	/** Who threw it, as a global id (the other server has no object for them): "<server>:<player>". */
	UPROPERTY()
	FString InstigatorId;
};

/** A send-time stamp: both servers run on one machine and share FPlatformTime's clock. */
USTRUCT()
struct FS2SStamp
{
	GENERATED_BODY()

	/** FPlatformTime::Seconds() on the authority when it changed the state this stamp travels with. */
	UPROPERTY()
	double SentAt = 0.0;

	UPROPERTY()
	int32 Seq = 0;

	/** Where the authority had the actor when it stamped (cm). */
	UPROPERTY()
	FVector AuthorityPos = FVector::ZeroVector;
};

/** One-way delays, collected in this process and summarised by the spike subsystem. */
namespace S2SStats
{
	CUBEWORLD_API void RecordLatency(FName Channel, double Seconds);
	/** Logs count, mean, p50, p95, max for every channel since the last call (bCumulative: since the start). */
	CUBEWORLD_API void LogLatency(const TCHAR* Prefix, bool bCumulative);

	/** "Authority", "SimulatedProxy", ... */
	CUBEWORLD_API FString RoleName(ENetRole Role);
	/** Who this process is: "A", "B" or "client", for the logs. */
	CUBEWORLD_API const FString& Who();
}
