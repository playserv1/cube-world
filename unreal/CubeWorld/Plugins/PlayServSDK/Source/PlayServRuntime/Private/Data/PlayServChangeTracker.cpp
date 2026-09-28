#include "Data/PlayServChangeTracker.h"
#include "Data/PlayServSerializer.h"
#include "Data/PlayServRecordIds.h"
#include "Data/PlayServCodegenRegistry.h"
#include "Core/PlayServLog.h"
#include "Dom/JsonValue.h"
#include "UObject/Object.h"
#include "UObject/Class.h"
#include "UObject/WeakObjectPtr.h"

TMap<TWeakObjectPtr<UObject>, TSharedPtr<FJsonObject>> FPlayServChangeTracker::Snapshots;
int32 FPlayServChangeTracker::InsertionCount = 0;

TWeakObjectPtr<UObject> FPlayServChangeTracker::MakeWeakKey(const UObject* Obj)
{
	return TWeakObjectPtr<UObject>(const_cast<UObject*>(Obj));
}

void FPlayServChangeTracker::PruneStaleSnapshots()
{
	for (auto It = Snapshots.CreateIterator(); It; ++It)
	{
		if (!It->Key.IsValid())
		{
			It.RemoveCurrent();
		}
	}
}

void FPlayServChangeTracker::TakeSnapshot(UObject* Object)
{
	if (!Object)
	{
		return;
	}

	TSharedPtr<FJsonObject> Json = FPlayServSerializer::UObjectToJson(Object);
	TakeSnapshot(Object, Json);
}

void FPlayServChangeTracker::TakeSnapshot(UObject* Object, const TSharedPtr<FJsonObject>& PrecomputedJson)
{
	if (!Object || !PrecomputedJson.IsValid())
	{
		return;
	}

	Snapshots.Add(MakeWeakKey(Object), PrecomputedJson);

	if (++InsertionCount >= 64)
	{
		InsertionCount = 0;
		PruneStaleSnapshots();
	}
}

bool FPlayServChangeTracker::HasSnapshot(const UObject* Object)
{
	return Snapshots.Contains(MakeWeakKey(Object));
}

void FPlayServChangeTracker::ClearSnapshot(const UObject* Object)
{
	Snapshots.Remove(MakeWeakKey(Object));
}

namespace
{
	bool ValuesEqual(const TSharedPtr<FJsonValue>& A, const TSharedPtr<FJsonValue>& B)
	{
		const bool bANull = !A.IsValid() || A->Type == EJson::Null;
		const bool bBNull = !B.IsValid() || B->Type == EJson::Null;
		if (bANull || bBNull)
		{
			return bANull == bBNull;
		}
		return FJsonValue::CompareEqual(*A, *B);
	}
}

bool FPlayServChangeTracker::ComputeMergePatch(
	const UObject* Object,
	TSharedPtr<FJsonObject>& OutMerge,
	TSharedPtr<FJsonObject>* OutCurrentJson)
{
	OutMerge = MakeShared<FJsonObject>();

	TSharedPtr<FJsonObject> CurrentJson = FPlayServSerializer::UObjectToJson(Object);
	if (!CurrentJson.IsValid())
	{
		return false;
	}

	if (OutCurrentJson)
	{
		*OutCurrentJson = CurrentJson;
	}

	const TSharedPtr<FJsonObject>* SnapshotPtr = Snapshots.Find(MakeWeakKey(Object));
	if (!SnapshotPtr || !SnapshotPtr->IsValid())
	{
		OutMerge = CurrentJson;
		return CurrentJson->Values.Num() >= 0;
	}

	const TSharedPtr<FJsonObject>& Snapshot = *SnapshotPtr;
	for (const auto& Pair : CurrentJson->Values)
	{
		if (!ValuesEqual(Snapshot->TryGetField(*Pair.Key), Pair.Value))
		{
			OutMerge->SetField(*Pair.Key, Pair.Value);
		}
	}

	UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ ChangeTracker: %s → %d changed field(s)"),
		Object ? *Object->GetClass()->GetName() : TEXT("<null>"), OutMerge->Values.Num());
	return OutMerge->Values.Num() > 0;
}

void FPlayServChangeTracker::ChangedTopLevelFields(const TSharedPtr<FJsonObject>& Before, const TSharedPtr<FJsonObject>& After, TArray<FString>& OutFields)
{
	if (!After.IsValid())
	{
		return;
	}
	for (const auto& Pair : After->Values)
	{
		const TSharedPtr<FJsonValue> Previous = Before.IsValid() ? Before->TryGetField(*Pair.Key) : nullptr;
		if (!ValuesEqual(Previous, Pair.Value))
		{
			OutFields.Add(FString(*Pair.Key));
		}
	}
	if (!Before.IsValid())
	{
		return;
	}
	for (const auto& Pair : Before->Values)
	{
		if (!After->HasField(*Pair.Key) && Pair.Value.IsValid() && Pair.Value->Type != EJson::Null)
		{
			OutFields.Add(FString(*Pair.Key));
		}
	}
}

void FPlayServChangeTracker::CollectDirtyEntities(const UObject* Root, TArray<FPlayServPendingWrite>& OutWrites)
{
	if (!Root || !FPlayServSerializer::IsPersistentClass(Root->GetClass()))
	{
		return;
	}

	TSharedPtr<FJsonObject> Merge;
	TSharedPtr<FJsonObject> CurrentJson;
	const FPlayServClassDescriptor* Descriptor = FPlayServCodegenRegistry::Get().FindForClass(Root->GetClass());
	const bool bSingleton = Descriptor != nullptr && Descriptor->bSingleton;
	const FString Id = FPlayServRecordIds::Get(Root);
	const bool bIsCreate = Id.IsEmpty() && !bSingleton;

	if (!ComputeMergePatch(Root, Merge, &CurrentJson) && !bIsCreate)
	{
		return;
	}

	FPlayServPendingWrite Write;
	Write.EntityType = Root->GetClass()->GetName();
	Write.Id = Id;
	Write.bIsCreate = bIsCreate;
	Write.bSingleton = bSingleton;
	Write.Body = bIsCreate ? CurrentJson : Merge;
	Write.CurrentJson = CurrentJson;
	Write.SourceObject = MakeWeakKey(Root);
	OutWrites.Add(MoveTemp(Write));
}
