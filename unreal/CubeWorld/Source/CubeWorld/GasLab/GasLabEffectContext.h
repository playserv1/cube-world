// The effect context of the lab, Lyra-style: the stock context plus who started the effect in words every server
// understands. The stock context names its instigator by object pointer, which means nothing on the next server; the
// player id and the region of the server the spec was made on survive the trip and let the target's server send the
// credit (damage dealt, a kill) back to the thrower's.
#pragma once

#include "CoreMinimal.h"
#include "GameplayEffectTypes.h"
#include "AbilitySystemGlobals.h"
#include "GasLabEffectContext.generated.h"

USTRUCT()
struct CUBEWORLD_API FGasLabEffectContext : public FGameplayEffectContext
{
	GENERATED_BODY()

	/** The instigating player's id, the same on every server. Empty for a world effect. */
	UPROPERTY() FString InstigatorId;
	/** The region of the server that made the spec (the thrower's), -1 when unknown. */
	UPROPERTY() int32 OriginRegion = -1;
	/** When the event that applies it happened (a creeper's blast), shared clock; 0 when not set. For the lab's timings. */
	UPROPERTY() double EventAt = 0;

	virtual UScriptStruct* GetScriptStruct() const override { return StaticStruct(); }
	virtual FGasLabEffectContext* Duplicate() const override;
	/**
	 * The legacy path writes the stock fields only. Under Iris this override is not called: the stock context serializer
	 * writes the base part and the UPROPERTYs above replicate on top of it, so clients get the ids too.
	 */
	virtual bool NetSerialize(FArchive& Ar, class UPackageMap* Map, bool& bOutSuccess) override;

	static FGasLabEffectContext* From(const FGameplayEffectContextHandle& Handle);
};

template<>
struct TStructOpsTypeTraits<FGasLabEffectContext> : public TStructOpsTypeTraitsBase2<FGasLabEffectContext>
{
	enum
	{
		WithNetSerializer = true,
		WithCopy = true,
	};
};

UCLASS()
class CUBEWORLD_API UGasLabAbilitySystemGlobals : public UAbilitySystemGlobals
{
	GENERATED_BODY()

public:
	virtual FGameplayEffectContext* AllocGameplayEffectContext() const override;
};
