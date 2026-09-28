// A tombstone where a player died, standing until they respawn: a grey headstone with a rounded top on a
// low base, "R.I.P." and the player's name carved on its face, and their name tag above it (web/tombstone.js).
// Its face looks along +X, as the player model does; the actor's yaw turns it.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "CubeTombstone.generated.h"

class UProceduralMeshComponent;
class UTextRenderComponent;

UCLASS()
class CUBEWORLD_API ACubeTombstone : public AActor
{
	GENERATED_BODY()

public:
	ACubeTombstone();
	virtual void Tick(float DeltaSeconds) override;

	void Setup(const FString& Name);
	/** Stands it at the feet (in blocks) with Minecraft's yaw, or takes it away. */
	void Show(bool bShow, const FVector& Feet = FVector::ZeroVector, double Yaw = 0);

private:
	UTextRenderComponent* Carve(const FString& Text, float Z, float Size, float Yaw);

	UPROPERTY() UProceduralMeshComponent* Stone = nullptr;
	UPROPERTY() UTextRenderComponent* Tag = nullptr;
};
