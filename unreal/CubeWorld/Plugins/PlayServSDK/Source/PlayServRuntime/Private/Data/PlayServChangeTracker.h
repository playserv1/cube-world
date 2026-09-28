#pragma once

#include "CoreMinimal.h"
#include "Data/PlayServWireTypes.h"
#include "Dom/JsonObject.h"
#include "UObject/WeakObjectPtr.h"

class FPlayServChangeTracker
{
public:
	static void TakeSnapshot(UObject* Object);

	static void TakeSnapshot(UObject* Object, const TSharedPtr<FJsonObject>& PrecomputedJson);

	static bool HasSnapshot(const UObject* Object);

	static void ClearSnapshot(const UObject* Object);

	static bool ComputeMergePatch(
		const UObject* Object,
		TSharedPtr<FJsonObject>& OutMerge,
		TSharedPtr<FJsonObject>* OutCurrentJson = nullptr);

	static void CollectDirtyEntities(const UObject* Root, TArray<FPlayServPendingWrite>& OutWrites);

	static void ChangedTopLevelFields(const TSharedPtr<FJsonObject>& Before, const TSharedPtr<FJsonObject>& After, TArray<FString>& OutFields);

private:
	static TWeakObjectPtr<UObject> MakeWeakKey(const UObject* Obj);
	static void PruneStaleSnapshots();

	static TMap<TWeakObjectPtr<UObject>, TSharedPtr<FJsonObject>> Snapshots;
	static int32 InsertionCount;
};
