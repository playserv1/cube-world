#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "SpikeTestEntities.generated.h"

// Spike fixtures. These exist to prove that the PlayServUht specifier route fires for classes
// declared INSIDE the plugin's test module. (USpikeAncestryEntity is a second specifier
// fixture — the class name is historical and kept for continuity with Codegen/spec.md.)
// They are not part of any schema and must never be pushed to the platform.

UCLASS(PlayServEntity)
class USpikeCodegenEntity : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY()
	FString Name;

	UPROPERTY()
	int32 Level = 1;
};

UCLASS(PlayServEntity)
class USpikeAncestryEntity : public UObject
{
	GENERATED_BODY()

public:
	UPROPERTY()
	int32 Score = 0;
};
