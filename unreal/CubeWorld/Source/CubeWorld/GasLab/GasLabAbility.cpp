#include "GasLab/GasLabAbility.h"
#include "GasLab/GasLabBorder.h"
#include "GasLab/GasLabCharacter.h"
#include "GasLab/GasLabClock.h"
#include "GasLab/GasLabCreeper.h"
#include "GasLab/GasLabEffects.h"
#include "GasLab/GasLabTags.h"
#include "AbilitySystemComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GasLabAbility)

UGasLabAbility_Throw::UGasLabAbility_Throw()
{
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;
	CooldownGameplayEffectClass = UGasLabGE_ThrowCooldown::StaticClass();
	FGameplayTagContainer Tags;
	Tags.AddTag(GasLabTags::Ability_Throw);
	SetAssetTags(Tags);
	// No throwing while being handed to another server: the prediction would belong to the old connection.
	ActivationBlockedTags.AddTag(UGasLabBorder::HandoffTag());
}

void UGasLabAbility_Throw::ActivateAbility(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
	if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}
	AGasLabCharacter* Thrower = Cast<AGasLabCharacter>(ActorInfo->AvatarActor.Get());
	if (HasAuthority(&ActivationInfo) && Thrower)
	{
		// Where it lands: the target a bot or scenario set, else straight ahead along the view.
		const FVector From = Thrower->GetActorLocation() + FVector(0, 0, 60.f);
		FVector Target;
		if (Thrower->ThrowTarget.IsSet())
		{
			Target = Thrower->ThrowTarget.GetValue();
			Thrower->ThrowTarget.Reset();
		}
		else
		{
			const FVector Ahead = Thrower->GetBaseAimRotation().Vector().GetSafeNormal2D();
			Target = Thrower->GetActorLocation() + Ahead * Range;
		}
		Target.Z = AGasLabCreeper::GroundZ;
		// A ballistic arc that lands at Target after FlightSeconds.
		const float T = FlightSeconds;
		FVector Velocity = (Target - From) / T;
		Velocity.Z = (Target.Z - From.Z - 0.5f * AGasLabCreeper::Gravity * T * T) / T;

		FGameplayEffectSpecHandle Blast = MakeOutgoingGameplayEffectSpec(Handle, ActorInfo, ActivationInfo, UGasLabGE_Blast::StaticClass(), GetAbilityLevel(Handle, ActorInfo));
		FGameplayEffectSpecHandle Burn = MakeOutgoingGameplayEffectSpec(Handle, ActorInfo, ActivationInfo, UGasLabGE_Burning::StaticClass(), GetAbilityLevel(Handle, ActorInfo));
		if (Blast.IsValid())
		{
			Blast.Data->SetSetByCallerMagnitude(GasLabTags::Data_Damage, BlastDamage);
			Blast.Data->SetSetByCallerMagnitude(GasLabTags::Data_Radius, AGasLabCreeper::Radius);
		}
		if (Burn.IsValid()) Burn.Data->SetSetByCallerMagnitude(GasLabTags::Data_BurnDamage, BurnDamage);

		UWorld* World = Thrower->GetWorld();
		const FTransform At(FRotator::ZeroRotator, From);
		AGasLabCreeper* Creeper = World->SpawnActorDeferred<AGasLabCreeper>(AGasLabCreeper::StaticClass(), At, Thrower, Thrower, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (Creeper)
		{
			static int32 Serial = 0;
			const UGasLabBorder* Border = UGasLabBorder::Get(Thrower);
			Creeper->Id = FString::Printf(TEXT("c%d-%d"), Border ? Border->GetRegion() : -1, ++Serial);
			Creeper->ThrowerId = Thrower->GetPlayerId();
			Creeper->OwnerRegion = Border ? Border->GetRegion() : -1;
			Creeper->Origin = From;
			Creeper->Velocity = Velocity;
			Creeper->LaunchAt = GasLabClock::Now();
			Creeper->BlastSpec = Blast;
			Creeper->BurnSpec = Burn;
			Creeper->FinishSpawning(At);
			UE_LOG(LogTemp, Log, TEXT("GASLAB throw creeper=%s by=%s target=%.0f,%.0f at=%.3f"), *Creeper->Id, *Creeper->ThrowerId, Target.X, Target.Y, Creeper->LaunchAt);
			if (UGasLabBorder* MutableBorder = UGasLabBorder::Get(Thrower)) MutableBorder->ShareCreeper(*Creeper);
		}
	}
	EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
}

FGameplayEffectContextHandle UGasLabAbility_Throw::MakeEffectContext(const FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo) const
{
	if (!ActorInfo || !ActorInfo->AbilitySystemComponent.IsValid()) return Super::MakeEffectContext(Handle, ActorInfo);
	FGameplayEffectContextHandle Context = ActorInfo->AbilitySystemComponent->MakeEffectContext();
	Context.SetAbility(this);
	if (UObject* Source = GetSourceObject(Handle, ActorInfo)) Context.AddSourceObject(Source);
	return Context;
}
