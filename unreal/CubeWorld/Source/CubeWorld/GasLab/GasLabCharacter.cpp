#include "GasLab/GasLabCharacter.h"
#include "GasLab/GasLabAbilitySystemComponent.h"
#include "GasLab/GasLabBorder.h"
#include "GasLab/GasLabClock.h"
#include "GasLab/GasLabPlayerState.h"
#include "GasLab/GasLabTags.h"
#include "CubeShapes.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "ProceduralMeshComponent.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(GasLabCharacter)

AGasLabCharacter::AGasLabCharacter()
{
	PrimaryActorTick.bCanEverTick = true;
	GetCapsuleComponent()->InitCapsuleSize(35.f, 90.f);
	bUseControllerRotationYaw = true;
	GetCharacterMovement()->MaxWalkSpeed = 450.f;
	GetCharacterMovement()->JumpZVelocity = 520.f;
	SetNetCullDistanceSquared(FMath::Square(20000.f));

	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(RootComponent);
	CameraBoom->TargetArmLength = 1100.f;
	CameraBoom->SetRelativeRotation(FRotator(-38.f, 0.f, 0.f));
	CameraBoom->SetRelativeLocation(FVector(0.f, 0.f, 120.f));
	// Fixed above and behind the player, turning with it: the whole scene around the player in view.
	CameraBoom->bUsePawnControlRotation = false;
	CameraBoom->bDoCollisionTest = false;
	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(CameraBoom);
}

UAbilitySystemComponent* AGasLabCharacter::GetAbilitySystemComponent() const
{
	return GetLabASC();
}

UGasLabAbilitySystemComponent* AGasLabCharacter::GetLabASC() const
{
	const AGasLabPlayerState* PS = GetPlayerState<AGasLabPlayerState>();
	return PS ? PS->GetLabASC() : nullptr;
}

FString AGasLabCharacter::GetPlayerId() const
{
	const AGasLabPlayerState* PS = GetPlayerState<AGasLabPlayerState>();
	return PS ? PS->LabId : FString();
}

bool AGasLabCharacter::IsGhost() const
{
	const AGasLabPlayerState* PS = GetPlayerState<AGasLabPlayerState>();
	return PS && PS->bGhost;
}

void AGasLabCharacter::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);
	InitAbilityActorInfo();
}

void AGasLabCharacter::OnRep_PlayerState()
{
	Super::OnRep_PlayerState();
	InitAbilityActorInfo();
}

void AGasLabCharacter::PawnClientRestart()
{
	Super::PawnClientRestart();
	InitAbilityActorInfo();
}

void AGasLabCharacter::InitAbilityActorInfo()
{
	AGasLabPlayerState* PS = GetPlayerState<AGasLabPlayerState>();
	if (!PS || !PS->GetLabASC()) return;
	PS->GetLabASC()->InitAbilityActorInfo(PS, this);
	if (IsGhost())
	{
		GetCharacterMovement()->DisableMovement();
		GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	}
}

void AGasLabCharacter::BeginPlay()
{
	Super::BeginPlay();
	if (GetNetMode() != NM_DedicatedServer)
	{
		Body = NewObject<UProceduralMeshComponent>(this, TEXT("Body"));
		Body->SetupAttachment(RootComponent);
		Body->RegisterComponent();
		Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Body->SetMaterial(0, FCubeShape::Material());
		Flames = NewObject<UProceduralMeshComponent>(this, TEXT("Flames"));
		Flames->SetupAttachment(RootComponent);
		Flames->RegisterComponent();
		Flames->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Flames->SetMaterial(0, FCubeShape::Material());
		FCubeShape Fire;
		Fire.bShaded = false;
		for (int32 I = 0; I < 5; I++)
		{
			const float A = I / 5.f * 2 * PI;
			Fire.Box(FVector(FMath::Cos(A) * 24.f, FMath::Sin(A) * 24.f, 110.f + (I % 2) * 14.f), FVector(9.f, 9.f, 18.f + (I % 3) * 6.f), FCubeShape::Hex(I % 2 ? TEXT("#f97316") : TEXT("#facc15")));
		}
		Fire.Commit(Flames);
		Flames->SetVisibility(false);
		Flash = NewObject<UProceduralMeshComponent>(this, TEXT("Flash"));
		Flash->SetupAttachment(RootComponent);
		Flash->RegisterComponent();
		Flash->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Flash->SetMaterial(0, FCubeShape::Material());
		FCubeShape White;
		White.bShaded = false;
		White.Box(FVector(0, 0, 0), FVector(48.f, 48.f, 100.f), FCubeShape::Hex(TEXT("#fff7ed")));
		White.Commit(Flash);
		Flash->SetVisibility(false);
		BuildBody();
	}
}

void AGasLabCharacter::BuildBody()
{
	if (!Body) return;
	const AGasLabPlayerState* PS = GetPlayerState<AGasLabPlayerState>();
	const int32 Region = PS ? PS->OwnerRegion : -1;
	const bool bGhostNow = PS && PS->bGhost;
	const int32 Key = Region * 2 + (bGhostNow ? 1 : 0);
	if (Key == BuiltFor) return;
	BuiltFor = Key;
	const FLinearColor Color = UGasLabBorder::RegionColor(Region);
	FCubeShape Shape;
	Shape.Box(FVector(0, 0, -20.f), FVector(28.f, 22.f, 55.f), Color);
	Shape.Box(FVector(0, 0, 62.f), FVector(24.f, 24.f, 24.f), FCubeShape::Hex(TEXT("#f1c27d")));
	Shape.Box(FVector(25.f, 0, 66.f), FVector(2.f, 16.f, 5.f), FCubeShape::Hex(TEXT("#1f2937")));
	// A ghost wears a ring at its feet in its owner's colour: it is someone the next server holds.
	if (bGhostNow)
	{
		for (int32 I = 0; I < 12; I++)
		{
			const float A = I / 12.f * 2 * PI, B = (I + 1) / 12.f * 2 * PI;
			Shape.Rod(FVector(FMath::Cos(A) * 50.f, FMath::Sin(A) * 50.f, -88.f), FVector(FMath::Cos(B) * 50.f, FMath::Sin(B) * 50.f, -88.f), 5.f, Color);
		}
	}
	Shape.Commit(Body);
}

void AGasLabCharacter::SetGhostTarget(const FVector& Location, const FVector& Velocity, float Yaw)
{
	if (GhostHeard == 0) SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
	GhostLocation = Location;
	GhostVelocity = Velocity;
	GhostYaw = Yaw;
	GhostHeard = FPlatformTime::Seconds();
}

void AGasLabCharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (HasAuthority() && IsGhost() && GhostHeard > 0)
	{
		const float Since = float(FMath::Min(FPlatformTime::Seconds() - GhostHeard, 0.25));
		const FVector Want = GhostLocation + GhostVelocity * Since;
		SetActorLocation(FMath::VInterpTo(GetActorLocation(), Want, DeltaSeconds, 20.f));
		SetActorRotation(FRotator(0.f, GhostYaw, 0.f));
	}
	if (Body)
	{
		BuildBody();
		if (Flash && FlashUntil > 0 && FPlatformTime::Seconds() > FlashUntil) { Flash->SetVisibility(false); FlashUntil = 0; }
	}
}

void AGasLabCharacter::HandleGameplayCue(AActor* Self, FGameplayTag Tag, EGameplayCueEvent::Type EventType, const FGameplayCueParameters& Parameters)
{
	IGameplayCueInterface::HandleGameplayCue(Self, Tag, EventType, Parameters);
	const TCHAR* Event = EventType == EGameplayCueEvent::OnActive ? TEXT("active") : EventType == EGameplayCueEvent::WhileActive ? TEXT("while")
		: EventType == EGameplayCueEvent::Executed ? TEXT("executed") : TEXT("removed");
	UE_LOG(LogTemp, Log, TEXT("GASLAB cue tag=%s event=%s on=%s ghost=%d at=%.3f"), *Tag.ToString(), Event, *GetPlayerId(), IsGhost() ? 1 : 0, GasLabClock::Now());
	if (Tag.MatchesTag(GasLabTags::GameplayCue_Burning) && Flames)
	{
		Flames->SetVisibility(EventType != EGameplayCueEvent::Removed);
	}
	if (Tag.MatchesTag(GasLabTags::GameplayCue_Creeper_Blast) && Flash && EventType == EGameplayCueEvent::Executed)
	{
		Flash->SetVisibility(true);
		FlashUntil = FPlatformTime::Seconds() + 0.25;
	}
}
