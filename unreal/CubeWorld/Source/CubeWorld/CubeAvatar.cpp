#include "CubeAvatar.h"
#include "CubeSpec.h"
#include "CubeTextures.h"
#include "CubeTombstone.h"
#include "Engine/World.h"
#include "ProceduralMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Kismet/GameplayStatics.h"
#include "Camera/PlayerCameraManager.h"

namespace
{
	constexpr float Px = CubeSpec::BlockCm / 16.f;   // one skin pixel, in cm, before the model scale

	struct FRect { int32 X, Y, W, H; };
	// The standard 64 × 64 layout: a box (u, v) of w × h × d unfolds as top and bottom above, then right, front, left, back.
	struct FRects { FRect Top, Bottom, Right, Front, Left, Back; };
	FRects RectsOf(int32 U, int32 V, int32 W, int32 H, int32 D)
	{
		return { { U + D, V, W, D }, { U + D + W, V, W, D }, { U, V + D, D, H }, { U + D, V + D, W, H }, { U + D + W, V + D, D, H }, { U + D + W + D, V + D, W, H } };
	}
}

ACubeAvatar::ACubeAvatar()
{
	PrimaryActorTick.bCanEverTick = true;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

USceneComponent* ACubeAvatar::MakePivot(const TCHAR* Name, const FVector& PositionPx)
{
	USceneComponent* Pivot = NewObject<USceneComponent>(this, Name);
	Pivot->SetupAttachment(RootComponent);
	Pivot->RegisterComponent();
	Pivot->SetRelativeLocation(PositionPx * Px);
	return Pivot;
}

// A box whose six faces read from the skin rectangles. The model's front is +X, its left +Y, its top +Z;
// the skin's w is the left-right extent, h the height, d the front-back depth.
UProceduralMeshComponent* ACubeAvatar::MakeBox(USceneComponent* Pivot, const TCHAR* Name, int32 U, int32 V, int32 W, int32 H, int32 D, const FVector& OffsetPx)
{
	UProceduralMeshComponent* Mesh = NewObject<UProceduralMeshComponent>(this, Name);
	Mesh->SetupAttachment(Pivot);
	Mesh->RegisterComponent();
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCastShadow(false);
	Mesh->SetMaterial(0, Skin);
	Mesh->SetRelativeLocation(OffsetPx * Px);

	const FRects R = RectsOf(U, V, W, H, D);
	const FVector Half(D * Px / 2, W * Px / 2, H * Px / 2);
	TArray<FVector> Vertices; TArray<int32> Triangles; TArray<FVector> Normals; TArray<FVector2D> UVs; TArray<FColor> Colors;
	struct FFace { FVector N; FVector C[4]; FRect Rect; float Light; };
	// Corners as bottom-left, bottom-right, top-right, top-left seen from outside.
	const FFace Faces[6] = {
		{ { 1, 0, 0 }, { { 1, 1, -1 }, { 1, -1, -1 }, { 1, -1, 1 }, { 1, 1, 1 } }, R.Front, 1.0f },
		{ { -1, 0, 0 }, { { -1, -1, -1 }, { -1, 1, -1 }, { -1, 1, 1 }, { -1, -1, 1 } }, R.Back, 0.8f },
		{ { 0, 1, 0 }, { { -1, 1, -1 }, { 1, 1, -1 }, { 1, 1, 1 }, { -1, 1, 1 } }, R.Left, 0.6f },
		{ { 0, -1, 0 }, { { 1, -1, -1 }, { -1, -1, -1 }, { -1, -1, 1 }, { 1, -1, 1 } }, R.Right, 0.6f },
		{ { 0, 0, 1 }, { { 1, 1, 1 }, { 1, -1, 1 }, { -1, -1, 1 }, { -1, 1, 1 } }, R.Top, 1.0f },
		{ { 0, 0, -1 }, { { -1, 1, -1 }, { -1, -1, -1 }, { 1, -1, -1 }, { 1, 1, -1 } }, R.Bottom, 0.5f },
	};
	for (const FFace& F : Faces)
	{
		const int32 Base = Vertices.Num();
		const float U0 = F.Rect.X / 64.f, U1 = (F.Rect.X + F.Rect.W) / 64.f, V0 = F.Rect.Y / 64.f, V1 = (F.Rect.Y + F.Rect.H) / 64.f;
		const FVector2D FaceUV[4] = { { U0, V1 }, { U1, V1 }, { U1, V0 }, { U0, V0 } };
		FVector Corners[4];
		for (int32 I = 0; I < 4; I++) { Corners[I] = F.C[I] * Half; Vertices.Add(Corners[I]); Normals.Add(F.N); UVs.Add(FaceUV[I]); const uint8 L = (uint8)(255 * F.Light); Colors.Add(FColor(L, L, L, 255)); }
		const bool bFlip = FVector::DotProduct(FVector::CrossProduct(Corners[1] - Corners[0], Corners[2] - Corners[0]), F.N) > 0;
		const int32 Order[6] = { 0, 1, 2, 0, 2, 3 }, Flipped[6] = { 0, 2, 1, 0, 3, 2 };
		for (int32 I : (bFlip ? Flipped : Order)) Triangles.Add(Base + I);
	}
	Mesh->CreateMeshSection(0, Vertices, Triangles, Normals, UVs, Colors, TArray<FProcMeshTangent>(), false);
	return Mesh;
}

void ACubeAvatar::Setup(const FString& InPlayerId, const FString& Name, UMaterialInterface* SkinBase)
{
	PlayerId = InPlayerId;
	Skin = UMaterialInstanceDynamic::Create(SkinBase, this);
	Skin->SetTextureParameterValue(TEXT("Skin"), FCubeTextures::PaintSkin(InPlayerId));
	Skin->SetVectorParameterValue(TEXT("Tint"), FLinearColor::White);
	RootComponent->SetRelativeScale3D(FVector(CubeSpec::ModelScale));

	// Pivots in pixels from the feet: neck at 24, shoulders at 22, hips at 12. Right is -Y.
	Head = MakePivot(TEXT("Head"), FVector(0, 0, 24));
	Body = MakePivot(TEXT("Body"), FVector(0, 0, 24));
	RightArm = MakePivot(TEXT("RightArm"), FVector(0, -6, 22));
	LeftArm = MakePivot(TEXT("LeftArm"), FVector(0, 6, 22));
	RightLeg = MakePivot(TEXT("RightLeg"), FVector(0, -2, 12));
	LeftLeg = MakePivot(TEXT("LeftLeg"), FVector(0, 2, 12));
	MakeBox(Head, TEXT("HeadMesh"), 0, 0, 8, 8, 8, FVector(0, 0, 4));
	MakeBox(Body, TEXT("BodyMesh"), 16, 16, 8, 12, 4, FVector(0, 0, -6));
	MakeBox(RightArm, TEXT("RightArmMesh"), 40, 16, 4, 12, 4, FVector(0, 0, -4));
	MakeBox(LeftArm, TEXT("LeftArmMesh"), 32, 48, 4, 12, 4, FVector(0, 0, -4));
	MakeBox(RightLeg, TEXT("RightLegMesh"), 0, 16, 4, 12, 4, FVector(0, 0, -6));
	MakeBox(LeftLeg, TEXT("LeftLegMesh"), 16, 48, 4, 12, 4, FVector(0, 0, -6));

	Tag = NewObject<UTextRenderComponent>(this, TEXT("Tag"));
	Tag->SetupAttachment(RootComponent);
	Tag->RegisterComponent();
	Tag->SetText(FText::FromString(Name));
	Tag->SetHorizontalAlignment(EHTA_Center);
	Tag->SetVerticalAlignment(EVRTA_TextBottom);
	Tag->SetWorldSize(28.f);
	Tag->SetTextRenderColor(FColor::White);
	Tag->SetRelativeLocation(FVector(0, 0, (CubeSpec::Height + 0.5) * CubeSpec::BlockCm / CubeSpec::ModelScale));
	Tag->SetAbsolute(false, true, true);

	Tomb = GetWorld()->SpawnActor<ACubeTombstone>();
	if (Tomb) Tomb->Setup(Name);
}

void ACubeAvatar::Destroyed()
{
	if (Tomb) Tomb->Destroy();
	Super::Destroyed();
}

namespace
{
	constexpr double SnapBlocks = 5;       // a respawn or a jump across regions is shown at once
	constexpr double MinInterval = 0.04, MaxInterval = 0.5;
	constexpr double Slack = 2;            // the walk takes a bit longer than an interval, so an uneven one rarely leaves it standing
}

void ACubeAvatar::SetTarget(double X, double Y, double Z, double InYaw, double InPitch, bool bInSneaking, double InHealth)
{
	bSneaking = bInSneaking; Health = InHealth;
	const double Now = FPlatformTime::Seconds();
	const bool bSame = X == TX && Y == TY && Z == TZ && InYaw == Yaw && InPitch == Pitch;
	if (bPlaced && bSame) return;
	if (bPlaced) Interval = FMath::Max(MinInterval, Interval * 0.8 + FMath::Min(Now - HeardAt, MaxInterval) * 0.2);
	HeardAt = Now;
	TX = X; TY = Y; TZ = Z; Yaw = InYaw; Pitch = InPitch;
	const FVector Target = FVector(X, Y, Z) * CubeSpec::BlockCm;
	if (!bPlaced || FVector::Dist(GetActorLocation(), Target) > SnapBlocks * CubeSpec::BlockCm)
	{
		SetActorLocation(Target);
		if (!bPlaced) Last = Target;
		ShownYaw = Yaw; ShownPitch = Pitch;
		ArriveAt = DrawnAt = Now;
		bPlaced = true;
	}
	else ArriveAt = Now + Interval * Slack;
}

void ACubeAvatar::Hurt()
{
	HurtUntil = FPlatformTime::Seconds() + CubeSpec::HurtTicks * CubeSpec::TickSeconds;
}

void ACubeAvatar::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!Head) return;
	// A dead player leaves the map; a tombstone with their name stands where they fell until they respawn.
	const bool bDead = IsDead();
	if (IsHidden() != bDead) SetActorHiddenInGame(bDead);
	if (Tomb) Tomb->Show(bDead, FVector(TX, TY, TZ), Yaw);
	// This frame covers its share of the way left, so the avatar arrives at ArriveAt whatever the frame rate.
	const double Now = FPlatformTime::Seconds(), Dt = Now - DrawnAt, Left = ArriveAt - DrawnAt;
	DrawnAt = Now;
	const double K = Left <= Dt ? 1 : FMath::Max(0.0, Dt / Left);
	const FVector Target = FVector(TX, TY, TZ) * CubeSpec::BlockCm;
	const FVector Position = FMath::Lerp(GetActorLocation(), Target, K);
	SetActorLocation(Position);
	ShownYaw += FMath::FindDeltaAngleRadians(ShownYaw, Yaw) * K;
	ShownPitch += (Pitch - ShownPitch) * K;
	// Minecraft's yaw 0 faces +y; Unreal's yaw 0 faces +x: the model turns by yaw + 90°.
	SetActorRotation(FRotator(0, FMath::RadiansToDegrees(ShownYaw) + 90.f, 0));

	const double Distance = FVector::Dist2D(Position, Last) / CubeSpec::BlockCm;
	Last = Position;
	Swing += Distance * 4;
	Amount += (FMath::Min(1.0, Distance * 4) - Amount) * 0.4;
	const double A = FMath::Cos(Swing * 0.6662), B = FMath::Cos(Swing * 0.6662 + PI);
	const float Deg = 180.f / PI;
	RightArm->SetRelativeRotation(FRotator(-(B * 2 * Amount * 0.5 + (bSneaking ? 0.4 : 0)) * Deg, 0, 0));
	LeftArm->SetRelativeRotation(FRotator(-(A * 2 * Amount * 0.5 + (bSneaking ? 0.4 : 0)) * Deg, 0, 0));
	RightLeg->SetRelativeRotation(FRotator(-A * 1.4 * Amount * Deg, 0, 0));
	LeftLeg->SetRelativeRotation(FRotator(-B * 1.4 * Amount * Deg, 0, 0));
	Head->SetRelativeRotation(FRotator(-FMath::RadiansToDegrees(ShownPitch), 0, 0));
	Body->SetRelativeRotation(FRotator(bSneaking ? -0.5 * Deg : 0, 0, 0));
	Head->SetRelativeLocation(FVector(0, 0, bSneaking ? 24 - 4.2 : 24) * Px);
	Body->SetRelativeLocation(FVector(0, 0, bSneaking ? 24 - 3.2 : 24) * Px);
	RightArm->SetRelativeLocation(FVector(0, -6, bSneaking ? 22 - 3.2 : 22) * Px);
	LeftArm->SetRelativeLocation(FVector(0, 6, bSneaking ? 22 - 3.2 : 22) * Px);

	const bool bHurt = FPlatformTime::Seconds() < HurtUntil;
	Skin->SetVectorParameterValue(TEXT("Tint"), bHurt ? FLinearColor(1, 0.45f, 0.45f) : FLinearColor::White);

	if (Tag)
	{
		Tag->SetVisibility(!bSneaking);
		if (const APlayerCameraManager* Camera = UGameplayStatics::GetPlayerCameraManager(this, 0))
		{
			const FVector ToCamera = Camera->GetCameraLocation() - Tag->GetComponentLocation();
			Tag->SetWorldRotation(FRotator(0, ToCamera.Rotation().Yaw, 0));
		}
	}
}
