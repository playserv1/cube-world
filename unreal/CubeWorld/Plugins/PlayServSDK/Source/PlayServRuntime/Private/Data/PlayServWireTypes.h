#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "UObject/WeakObjectPtr.h"

struct FPlayServPendingWrite
{
	FString EntityType;
	FString Id;
	bool bIsCreate = false;
	bool bSingleton = false;

	TSharedPtr<FJsonObject> Body;

	TSharedPtr<FJsonObject> CurrentJson;

	TWeakObjectPtr<UObject> SourceObject;
};
