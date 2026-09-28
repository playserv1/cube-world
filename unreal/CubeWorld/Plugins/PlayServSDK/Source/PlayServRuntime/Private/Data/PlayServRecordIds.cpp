#include "Data/PlayServRecordIds.h"

TMap<TWeakObjectPtr<const UObject>, FString> FPlayServRecordIds::IdsByObject;
int32 FPlayServRecordIds::InsertionsSincePrune = 0;

FString FPlayServRecordIds::Get(const UObject* Object)
{
	if (!Object)
	{
		return FString();
	}
	const FString* Found = IdsByObject.Find(TWeakObjectPtr<const UObject>(Object));
	return Found ? *Found : FString();
}

void FPlayServRecordIds::Bind(const UObject* Object, const FString& RecordId)
{
	if (!Object)
	{
		return;
	}
	IdsByObject.Add(TWeakObjectPtr<const UObject>(Object), RecordId);

	if (++InsertionsSincePrune >= 64)
	{
		InsertionsSincePrune = 0;
		for (auto It = IdsByObject.CreateIterator(); It; ++It)
		{
			if (!It->Key.IsValid())
			{
				It.RemoveCurrent();
			}
		}
	}
}

void FPlayServRecordIds::Forget(const UObject* Object)
{
	if (Object)
	{
		IdsByObject.Remove(TWeakObjectPtr<const UObject>(Object));
	}
}
