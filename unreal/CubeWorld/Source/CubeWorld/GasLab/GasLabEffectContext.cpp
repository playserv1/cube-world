#include "GasLab/GasLabEffectContext.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GasLabEffectContext)

FGasLabEffectContext* FGasLabEffectContext::Duplicate() const
{
	FGasLabEffectContext* Copy = new FGasLabEffectContext();
	*Copy = *this;
	if (GetHitResult())
	{
		// A deep copy of the hit result, as the stock context makes.
		Copy->AddHitResult(*GetHitResult(), true);
	}
	return Copy;
}

bool FGasLabEffectContext::NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess)
{
	return FGameplayEffectContext::NetSerialize(Ar, Map, bOutSuccess);
}

FGasLabEffectContext* FGasLabEffectContext::From(const FGameplayEffectContextHandle& Handle)
{
	FGameplayEffectContext* Base = const_cast<FGameplayEffectContext*>(Handle.Get());
	if (Base && Base->GetScriptStruct()->IsChildOf(FGasLabEffectContext::StaticStruct()))
	{
		return static_cast<FGasLabEffectContext*>(Base);
	}
	return nullptr;
}

FGameplayEffectContext* UGasLabAbilitySystemGlobals::AllocGameplayEffectContext() const
{
	return new FGasLabEffectContext();
}
