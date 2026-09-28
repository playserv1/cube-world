// Another player: Minecraft's model (head 8×8×8, body 8×12×4, arms and legs 4×12×4 pixels, 16 pixels to
// the block, drawn at 15/16) wearing the skin painted from their id, with a walk cycle, a name tag,
// the sneaking pose and a red flash when hurt. The model faces +X; the actor's yaw turns it.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CubeAvatar.generated.h"

class UProceduralMeshComponent;
class UMaterialInstanceDynamic;
class UTextRenderComponent;
class USceneComponent;

UCLASS()
class CUBEWORLD_API ACubeAvatar : public AActor
{
	GENERATED_BODY()

public:
	ACubeAvatar();
	virtual void Tick(float DeltaSeconds) override;

	void Setup(const FString& PlayerId, const FString& Name, UMaterialInterface* SkinBase);
	/** Server coordinates in blocks; yaw and pitch are Minecraft's, in radians. */
	void SetTarget(double X, double Y, double Z, double Yaw, double Pitch, bool bSneaking, double Health);
	void Hurt();

	FString PlayerId;
	double TX = 0, TY = 0, TZ = 0, Yaw = 0, Pitch = 0, Health = 20;
	bool bSneaking = false;

private:
	UProceduralMeshComponent* MakeBox(USceneComponent* Pivot, const TCHAR* Name, int32 U, int32 V, int32 W, int32 H, int32 D, const FVector& OffsetPx);
	USceneComponent* MakePivot(const TCHAR* Name, const FVector& PositionPx);

	UPROPERTY() USceneComponent* Head = nullptr;
	UPROPERTY() USceneComponent* Body = nullptr;
	UPROPERTY() USceneComponent* RightArm = nullptr;
	UPROPERTY() USceneComponent* LeftArm = nullptr;
	UPROPERTY() USceneComponent* RightLeg = nullptr;
	UPROPERTY() USceneComponent* LeftLeg = nullptr;
	UPROPERTY() UTextRenderComponent* Tag = nullptr;
	UPROPERTY() UMaterialInstanceDynamic* Skin = nullptr;
	FVector Last = FVector::ZeroVector;
	double Swing = 0, Amount = 0;
	double HurtUntil = 0;
	bool bPlaced = false;
};
