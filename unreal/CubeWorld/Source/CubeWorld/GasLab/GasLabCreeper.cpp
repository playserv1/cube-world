#include "GasLab/GasLabCreeper.h"
#include "GasLab/GasLabAbilitySystemComponent.h"
#include "GasLab/GasLabBorder.h"
#include "GasLab/GasLabCharacter.h"
#include "GasLab/GasLabClock.h"
#include "GasLab/GasLabEffectContext.h"
#include "CubeBombs.h"
#include "CubeSpec.h"
#include "EngineUtils.h"
#include "Net/UnrealNetwork.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GasLabCreeper)

AGasLabCreeper::AGasLabCreeper()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = true;
	SetReplicatingMovement(false);
	bAlwaysRelevant = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void AGasLabCreeper::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AGasLabCreeper, Id);
	DOREPLIFETIME(AGasLabCreeper, ThrowerId);
	DOREPLIFETIME(AGasLabCreeper, OwnerRegion);
	DOREPLIFETIME(AGasLabCreeper, Origin);
	DOREPLIFETIME(AGasLabCreeper, Velocity);
	DOREPLIFETIME(AGasLabCreeper, LaunchAt);
	DOREPLIFETIME(AGasLabCreeper, bExploded);
	DOREPLIFETIME(AGasLabCreeper, ExplodedAt);
	DOREPLIFETIME(AGasLabCreeper, bGhost);
}

FVector AGasLabCreeper::PositionAt(double T) const
{
	const float Dt = float(FMath::Max(0.0, T - LaunchAt));
	const FVector O = Origin, V = Velocity;
	const float Z = O.Z + V.Z * Dt + 0.5f * Gravity * Dt * Dt;
	if (Z > GroundZ) return FVector(O.X + V.X * Dt, O.Y + V.Y * Dt, Z);
	// It lands where z(t) = GroundZ and stays there.
	const float A = 0.5f * Gravity, B = V.Z, C = O.Z - GroundZ;
	const float Disc = FMath::Max(0.f, B * B - 4.f * A * C);
	const float Land = (-B - FMath::Sqrt(Disc)) / (2.f * A);
	return FVector(O.X + V.X * Land, O.Y + V.Y * Land, GroundZ);
}

void AGasLabCreeper::BeginPlay()
{
	Super::BeginPlay();
	if (GetNetMode() != NM_DedicatedServer)
	{
		Look = GetWorld()->SpawnActor<ACubeBomb>(GetActorLocation(), FRotator::ZeroRotator);
		if (Look)
		{
			Look->State = TEXT("flying");
			Look->AttachToActor(this, FAttachmentTransformRules::KeepRelativeTransform);
			Look->SetActorRelativeLocation(FVector(0, 0, -40.f));
			Look->SetActorScale3D(FVector(3.f));
		}
		if (bExploded) OnRep_Exploded();
	}
}

void AGasLabCreeper::EndPlay(const EEndPlayReason::Type Reason)
{
	if (Look) Look->Destroy();
	Super::EndPlay(Reason);
}

void AGasLabCreeper::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	const double Now = GasLabClock::Now();
	if (!bExploded) SetActorLocation(PositionAt(Now));
	if (Look) Look->Animate(Now);
	if (!HasAuthority()) return;
	if (!bExploded && !bGhost && Now - LaunchAt >= Fuse) Explode();
	// A ghost whose owner never says it blew (a lost message, a server gone) goes quietly.
	if (!bExploded && bGhost && Now - LaunchAt > Fuse + 5.0) Destroy();
	if (bExploded && FPlatformTime::Seconds() - ExplodedLocalAt > 3.0) Destroy();
}

int32 AGasLabCreeper::ApplyAround(const FVector& Center, const FGameplayEffectSpec& Spec, bool bOwnOnly, const TCHAR* Via)
{
	int32 N = 0;
	for (TActorIterator<AGasLabCharacter> It(GetWorld()); It; ++It)
	{
		AGasLabCharacter* Target = *It;
		UGasLabAbilitySystemComponent* ASC = Target ? Target->GetLabASC() : nullptr;
		if (!ASC || (bOwnOnly && Target->IsGhost())) continue;
		const float Distance = FVector::Dist(Center, Target->GetActorLocation());
		if (Distance > Radius) continue;
		UE_LOG(LogTemp, Log, TEXT("GASLAB apply creeper=%s effect=%s target=%s ghost=%d dist=%.0f via=%s at=%.3f"),
			*Id, *GetNameSafe(Spec.Def ? Spec.Def->GetClass() : nullptr), *Target->GetPlayerId(), Target->IsGhost() ? 1 : 0, Distance, Via, GasLabClock::Now());
		// The stock call a projectile makes; a ghost's ability system forwards it to the player's own server.
		ASC->ApplyGameplayEffectSpecToSelf(Spec);
		N++;
	}
	return N;
}

void AGasLabCreeper::Explode()
{
	if (bExploded) return;
	UGasLabBorder* Border = UGasLabBorder::Get(this);
	const FVector Center = GetActorLocation();
	const double Now = GasLabClock::Now();
	bExploded = true;
	ExplodedAt = Center;
	ExplodedLocalAt = FPlatformTime::Seconds();
	const bool bArea = Border && Border->GetMode() == EGasLabMode::Area;
	UE_LOG(LogTemp, Log, TEXT("GASLAB explode creeper=%s by=%s region=%d pos=%.0f,%.0f mode=%s at=%.3f"),
		*Id, *ThrowerId, OwnerRegion, Center.X, Center.Y, bArea ? TEXT("area") : TEXT("forward"), Now);
	for (FGameplayEffectSpecHandle* Handle : { &BlastSpec, &BurnSpec })
	{
		if (!Handle->IsValid()) continue;
		FGameplayEffectContextHandle Context = Handle->Data->GetEffectContext();
		Context.AddOrigin(Center);
		if (FGasLabEffectContext* Lab = FGasLabEffectContext::From(Context)) Lab->EventAt = Now;
	}
	if (BlastSpec.IsValid()) ApplyAround(Center, *BlastSpec.Data, bArea, TEXT("local"));
	if (BurnSpec.IsValid()) ApplyAround(Center, *BurnSpec.Data, bArea, TEXT("local"));
	if (Border) Border->ShareCreeper(*this);
	ForceNetUpdate();
}

void AGasLabCreeper::ExplodedElsewhere(const FVector& At, double When)
{
	if (bExploded) return;
	bExploded = true;
	ExplodedAt = At;
	ExplodedLocalAt = FPlatformTime::Seconds();
	SetActorLocation(At);
	UE_LOG(LogTemp, Log, TEXT("GASLAB ghost-explode creeper=%s owner=%d lag=%.1fms at=%.3f"), *Id, OwnerRegion, (GasLabClock::Now() - When) * 1000.0, GasLabClock::Now());
	// Area mode: the creeper carried its effects here; this server applies them to its own players.
	if (GhostBlast.IsSet() || GhostBurn.IsSet())
	{
		for (const TOptional<FGasLabWireSpec>* Wire : { &GhostBlast, &GhostBurn })
		{
			if (!Wire->IsSet()) continue;
			FGasLabWireSpec Copy = Wire->GetValue();
			Copy.bHasOrigin = true;
			Copy.Origin = At;
			Copy.EventAt = When;
			if (TSharedPtr<FGameplayEffectSpec> Spec = Copy.Build(nullptr)) ApplyAround(At, *Spec, true, TEXT("area"));
		}
	}
	ForceNetUpdate();
}

void AGasLabCreeper::OnRep_Exploded()
{
	if (bExploded) Boom();
}

void AGasLabCreeper::Boom()
{
	if (Look) Look->SetActorHiddenInGame(true);
	ACubeBurst::Explosion(GetWorld(), FVector(ExplodedAt) / CubeSpec::BlockCm);
	UE_LOG(LogTemp, Log, TEXT("GASLAB seen-explode creeper=%s owner=%d ghost=%d at=%.3f"), *Id, OwnerRegion, bGhost ? 1 : 0, GasLabClock::Now());
}
