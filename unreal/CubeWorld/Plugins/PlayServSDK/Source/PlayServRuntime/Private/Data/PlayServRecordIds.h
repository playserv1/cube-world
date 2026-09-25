#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"

class FPlayServRecordIds
{
public:
	static FString Get(const UObject* Object);
	static void Bind(const UObject* Object, const FString& RecordId);
	static void Forget(const UObject* Object);

private:
	static TMap<TWeakObjectPtr<const UObject>, FString> IdsByObject;
	static int32 InsertionsSincePrune;
};
