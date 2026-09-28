#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

class FPlayServSerializer
{
public:
	static TSharedPtr<FJsonObject> UObjectToJson(const UObject* Object);

	static bool JsonToUObject(const TSharedPtr<FJsonObject>& JsonObject, UObject* Object);

	static TSharedPtr<FJsonValue> PropertyToJsonValue(FProperty* Property, const void* ValuePtr, const UObject* Owner);

	static bool JsonValueToProperty(const TSharedPtr<FJsonValue>& JsonValue, FProperty* Property, void* ValuePtr, UObject* Owner);

	static FString StructMemberWireName(const FProperty* Property);

	static bool IsPersistentClass(const UClass* Class);

#if !UE_BUILD_SHIPPING
	static bool bTestForceReflectionWalk;
#endif
};
