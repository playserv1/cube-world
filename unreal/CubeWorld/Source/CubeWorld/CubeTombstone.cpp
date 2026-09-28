#include "CubeTombstone.h"
#include "CubeShapes.h"
#include "CubeSpec.h"
#include "ProceduralMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Camera/PlayerCameraManager.h"

namespace
{
	// The headstone in centimetres: 62 wide, 78 high and 16 thick over a 12 high base.
	constexpr float W = 62, H = 78, D = 16, Base = 12;
}

ACubeTombstone::ACubeTombstone()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

UTextRenderComponent* ACubeTombstone::Carve(const FString& Text, float Z, float Size, float Yaw)
{
	UTextRenderComponent* Carving = NewObject<UTextRenderComponent>(this);
	Carving->SetupAttachment(RootComponent);
	Carving->RegisterComponent();
	Carving->SetText(FText::FromString(Text));
	Carving->SetHorizontalAlignment(EHTA_Center);
	Carving->SetVerticalAlignment(EVRTA_TextCenter);
	Carving->SetTextRenderColor(FColor::FromHex(TEXT("#3f4145")));
	Carving->SetWorldSize(Size);
	// A long name is set smaller until it fits the face.
	while (Carving->GetTextLocalSize().Y > W * 0.87f && Size > 5) Carving->SetWorldSize(Size -= 1);
	const float X = Yaw == 0 ? D / 2 + 0.3f : -D / 2 - 0.3f;
	Carving->SetRelativeLocationAndRotation(FVector(X, 0, Z), FRotator(0, Yaw, 0));
	return Carving;
}

void ACubeTombstone::Setup(const FString& Name)
{
	Stone = NewObject<UProceduralMeshComponent>(this, TEXT("Stone"));
	Stone->SetupAttachment(RootComponent);
	Stone->RegisterComponent();
	Stone->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Stone->SetCastShadow(false);
	Stone->bUseAsyncCooking = false;
	Stone->SetMaterial(0, FCubeShape::Material());
	FCubeShape Shape;
	const FLinearColor Grey = FCubeShape::Hex(TEXT("#8d9096"));
	Shape.Box(FVector(0, 0, Base + H / 2), FVector(D / 2, W / 2, H / 2), Grey);
	Shape.Cylinder(FVector(0, 0, Base + H), FVector::XAxisVector, W / 2, D, 20, Grey, 0, PI);
	Shape.Box(FVector(0, 0, Base / 2), FVector((D + 30) / 2, (W + 24) / 2, Base / 2), FCubeShape::Hex(TEXT("#6b6e73")));
	Shape.Commit(Stone);

	// The face in the browser is 160 pixels to 78 cm: "R.I.P." at 52 pixels from the top, the name at 100.
	for (const float Yaw : { 0.f, 180.f })
	{
		Carve(TEXT("R.I.P."), Base + H * (1 - 52.f / 160), 13, Yaw);
		Carve(Name, Base + H * (1 - 100.f / 160), 11, Yaw);
	}

	Tag = NewObject<UTextRenderComponent>(this, TEXT("Tag"));
	Tag->SetupAttachment(RootComponent);
	Tag->RegisterComponent();
	Tag->SetText(FText::FromString(Name));
	Tag->SetHorizontalAlignment(EHTA_Center);
	Tag->SetVerticalAlignment(EVRTA_TextBottom);
	Tag->SetWorldSize(28.f);
	Tag->SetTextRenderColor(FColor::White);
	Tag->SetRelativeLocation(FVector(0, 0, 150));
	Tag->SetAbsolute(false, true, true);
	SetActorHiddenInGame(true);
}

void ACubeTombstone::Show(bool bShow, const FVector& Feet, double Yaw)
{
	if (bShow) SetActorLocationAndRotation(Feet * CubeSpec::BlockCm, FRotator(0, FMath::RadiansToDegrees(Yaw) + 90.f, 0));
	if (IsHidden() == bShow) SetActorHiddenInGame(!bShow);
}

void ACubeTombstone::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!Tag || IsHidden()) return;
	if (const APlayerCameraManager* Camera = UGameplayStatics::GetPlayerCameraManager(this, 0))
		Tag->SetWorldRotation(FRotator(0, (Camera->GetCameraLocation() - Tag->GetComponentLocation()).Rotation().Yaw, 0));
}
