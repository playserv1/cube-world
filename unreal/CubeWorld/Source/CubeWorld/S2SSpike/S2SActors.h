// The actors server A mirrors to server B in the spike.
//
//   AS2SProjectile: flies along X across the border at x = 0 (A's region is x < 0, B's is x >= 0) and carries a
//                   gameplay effect as FS2SEffectPayload. Every tick on A it stamps the send time, so B measures the
//                   one-way delay of each update.
//   AS2SAscActor:   a stock UAbilitySystemComponent with one attribute set (Health), a loose tag, a looping cue added on
//                   A, an instant damage effect A applies every second and a burst cue A executes every two seconds.
//
// Both are bAlwaysRelevant: the S2S connection has no PlayerController and no view target, so Iris's spatial filter
// would have no view to place them in.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemInterface.h"
#include "AttributeSet.h"
#include "GameplayEffect.h"
#include "S2STypes.h"
#include "S2SActors.generated.h"

UCLASS()
class AS2SProjectile : public AActor, public IS2SMirrored
{
	GENERATED_BODY()

public:
	AS2SProjectile();

	virtual bool ShouldMirrorToPeers() const override { return bMirrorToPeers; }
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	virtual void Tick(float DeltaSeconds) override;

	/** Set before BeginPlay (deferred spawn): false keeps it on this server's own clients only. */
	UPROPERTY()
	bool bMirrorToPeers = true;

	/** The process that spawned it ("A", "B"): tells a client which server's actor it is looking at. */
	UPROPERTY(Replicated)
	FString Origin;

	/** cm/s; the authority moves it, the copies follow ReplicatedMovement. */
	UPROPERTY(Replicated)
	FVector Velocity = FVector(1000.0, 0.0, 0.0);

	/** Half the length of the lane it flies along, centred on the border (cm). */
	UPROPERTY(Replicated)
	double HalfLane = 20000.0;

	UPROPERTY(Replicated)
	FS2SEffectPayload Payload;

	UPROPERTY(ReplicatedUsing = OnRep_Stamp)
	FS2SStamp Stamp;

	/** On B in the localswap relay: a copy whose roles were flipped locally so B's game driver replicates it. */
	bool bS2SCopy = false;

	/** Number of updates received (copies only), and the last stamp seen. */
	int32 ReceivedUpdates = 0;
	int32 LastSeq = 0;
	int32 SkippedSeqs = 0;

private:
	UFUNCTION()
	void OnRep_Stamp();
};

UCLASS()
class US2SAttributeSet : public UAttributeSet
{
	GENERATED_BODY()

public:
	ATTRIBUTE_ACCESSORS_BASIC(US2SAttributeSet, Health)

	UPROPERTY(ReplicatedUsing = OnRep_Health)
	FGameplayAttributeData Health;

	/** Stamped by the authority whenever an effect executes on Health; the copies measure attribute latency with it. */
	UPROPERTY(Replicated)
	FS2SStamp ChangeStamp;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data) override;

	int32 ReceivedChanges = 0;

private:
	UFUNCTION()
	void OnRep_Health(const FGameplayAttributeData& OldValue);

	int32 LastSeenSeq = 0;
};

/** Instant: Health += SetByCaller(SetByCaller.S2S.Damage). */
UCLASS()
class US2SDamageEffect : public UGameplayEffect
{
	GENERATED_BODY()

public:
	US2SDamageEffect();
};

/** Infinite: Health += 100 while active. Shows whether active effects reach the copy (Full) or not (Mixed). */
UCLASS()
class US2SBuffEffect : public UGameplayEffect
{
	GENERATED_BODY()

public:
	US2SBuffEffect();
};

UCLASS()
class AS2SAscActor : public AActor, public IS2SMirrored, public IAbilitySystemInterface
{
	GENERATED_BODY()

public:
	AS2SAscActor();

	virtual bool ShouldMirrorToPeers() const override { return bMirrorToPeers; }
	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override { return AbilitySystem; }
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type Reason) override;
	virtual void Tick(float DeltaSeconds) override;

	UPROPERTY()
	bool bMirrorToPeers = true;

	/** The authority applies damage and executes the burst cue on a timer (A's mirrored actor); B's own target does not. */
	UPROPERTY()
	bool bRunAuthorityTimers = true;

	/** The process that spawned it ("A", "B"). */
	UPROPERTY(Replicated)
	FString Origin;

	/** On B in the localswap relay: a copy whose roles were flipped locally so B's game driver replicates it. */
	bool bS2SCopy = false;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UAbilitySystemComponent> AbilitySystem;

	UPROPERTY()
	TObjectPtr<US2SAttributeSet> Attributes;

	/** One line: role, health (current/base), owned tags, active effects, the ASC's view of its authority. */
	FString Describe() const;

private:
	double NextDamageAt = 0.0;
	double NextBurstAt = 0.0;
};
