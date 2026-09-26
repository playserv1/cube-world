#include "Data/PlayServData.h"
#include "Data/PlayServFilter.h"
#include "Data/PlayServSerializer.h"
#include "Data/PlayServChangeTracker.h"
#include "Data/PlayServRecordIds.h"
#include "Data/PlayServCodegenRegistry.h"
#include "Data/PlayServWireTypes.h"
#include "Auth/PlayServAuth.h"
#include "Core/PlayServHttp.h"
#include "Core/PlayServLog.h"
#include "Core/PlayServSubsystem.h"
#include "Realtime/PlayServDataflow.h"
#include "Rooms/PlayServRooms.h"
#include "GameFramework/PlayerController.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "Data/PlayServBatchCounter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/DateTime.h"
#include "UObject/UnrealType.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/Object.h"
#include "UObject/WeakObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/Package.h"

static void ResetPersistedFields(UObject* Instance)
{
	const UObject* Defaults = Instance->GetClass()->GetDefaultObject();
	if (const FPlayServClassDescriptor* Descriptor = FPlayServCodegenRegistry::Get().FindForClass(Instance->GetClass()))
	{
		for (const FPlayServFieldDesc& Field : Descriptor->Fields)
		{
			if (Field.Property != nullptr)
			{
				Field.Property->CopyCompleteValue_InContainer(Instance, Defaults);
			}
		}
		return;
	}
	for (TFieldIterator<FProperty> It(Instance->GetClass()); It; ++It)
	{
		if (!It->HasAnyPropertyFlags(CPF_Transient | CPF_EditorOnly))
		{
			It->CopyCompleteValue_InContainer(Instance, Defaults);
		}
	}
}

static TSharedPtr<FJsonObject> ApplyFetchedJson(UObject* Instance, const TSharedPtr<FJsonObject>& Result)
{
	ResetPersistedFields(Instance);

	const TSharedPtr<FJsonObject>* PayloadObj;
	if (Result->TryGetObjectField(TEXT("payload"), PayloadObj))
	{
		FPlayServSerializer::JsonToUObject(*PayloadObj, Instance);
	}
	else
	{
		FPlayServSerializer::JsonToUObject(Result, Instance);
	}

	TSharedPtr<FJsonObject> Snapshot = FPlayServSerializer::UObjectToJson(Instance);
	FPlayServChangeTracker::TakeSnapshot(Instance, Snapshot);
	return Snapshot;
}

static void CallFieldFunction(UObject* Instance, UFunction* Function, const FProperty* Field, const void* OldValue)
{
	if (Function->NumParms == 0 || OldValue == nullptr)
	{
		Instance->ProcessEvent(Function, nullptr);
		return;
	}

	uint8* Params = static_cast<uint8*>(FMemory_Alloca_Aligned(Function->ParmsSize, Function->GetMinAlignment()));
	FMemory::Memzero(Params, Function->ParmsSize);
	for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		It->InitializeValue_InContainer(Params);
	}

	TFieldIterator<FProperty> FirstParam(Function);
	FProperty* Param = FirstParam ? *FirstParam : nullptr;
	const FBoolProperty* FieldBool = CastField<FBoolProperty>(Field);
	FBoolProperty* ParamBool = CastField<FBoolProperty>(Param);
	if (FieldBool != nullptr && ParamBool != nullptr)
	{
		ParamBool->SetPropertyValue_InContainer(Params, FieldBool->GetPropertyValue(OldValue));
	}
	else if (Param != nullptr)
	{
		Param->CopyCompleteValue(Param->ContainerPtrToValuePtr<void>(Params), OldValue);
	}

	Instance->ProcessEvent(Function, Params);

	for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
	{
		It->DestroyValue_InContainer(Params);
	}
}

struct UPlayServData::FAppliedChange
{
	struct FFieldCall
	{
		UFunction* Function = nullptr;
		const FProperty* Field = nullptr;
		FString WireName;
		void* OldValue = nullptr;
	};

	TWeakObjectPtr<UObject> Instance;
	TArray<FName> ChangedFields;
	TArray<FFieldCall> Calls;
	TArray<TStrongObjectPtr<UObject>> KeepAlive;

	FAppliedChange() = default;
	FAppliedChange(const FAppliedChange&) = delete;
	FAppliedChange& operator=(const FAppliedChange&) = delete;

	~FAppliedChange()
	{
		for (FFieldCall& Call : Calls)
		{
			Release(Call);
		}
	}

	static void Release(FFieldCall& Call)
	{
		if (Call.OldValue != nullptr)
		{
			Call.Field->DestroyValue(Call.OldValue);
			FMemory::Free(Call.OldValue);
			Call.OldValue = nullptr;
		}
	}
};

void UPlayServData::Init(TSharedPtr<FPlayServHttp> InHttp)
{
	Http = InHttp;
}

void UPlayServData::Shutdown()
{
	if (Dataflow.IsValid())
	{
		Dataflow->Shutdown();
		Dataflow.Reset();
	}
	SubscriptionsByHandle.Empty();

	Http.Reset();

	TMap<FObjectKey, FEntityOpQueue> Draining = MoveTemp(EntityOpQueues);
	EntityOpQueues.Reset();
	for (TPair<FObjectKey, FEntityOpQueue>& Pair : Draining)
	{
		for (FPendingEntityOp& Op : Pair.Value.Pending)
		{
			for (FPlayServSimpleCallback& Cb : Op.Callbacks)
			{
				Cb.ExecuteIfBound(false, FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("PlayServ data module shut down; queued operation cancelled.")));
			}
		}
	}
}

void UPlayServData::EndRealtimeSession()
{
	if (Dataflow.IsValid())
	{
		Dataflow->Shutdown();
		Dataflow.Reset();
	}
	if (SubscriptionsByHandle.Num() > 0)
	{
		UE_LOG(LogPlayServ, Display, TEXT("PlayServ Data: logout — ending %d subscription handle(s)"), SubscriptionsByHandle.Num());
		SubscriptionsByHandle.Empty();
	}
}

void UPlayServData::ResolveEntityId(const FString& EntityType, TFunction<void(const FString&, const FPlayServError&)> OnResolved)
{
	if (const FString* Cached = EntityIdCache.Find(EntityType))
	{
		OnResolved(*Cached, FPlayServError::Success());
		return;
	}

	if (!Http.IsValid())
	{
		OnResolved(FString(), FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("PlayServ HTTP not initialized")));
		return;
	}

	PendingResolutions.Emplace(EntityType, MoveTemp(OnResolved));
	if (bEntityIdFetchInFlight)
	{
		return;
	}
	bEntityIdFetchInFlight = true;

	TWeakObjectPtr<UPlayServData> WeakThis(this);
	Http->Request(EPlayServHttpVerb::Get, TEXT("/data/tables"), nullptr, FPlayServV2Callback::CreateLambda(
		[WeakThis](bool bSuccess, const FPlayServHttpResponse& Response, const FPlayServError& Error)
		{
			UPlayServData* Self = WeakThis.Get();
			if (!Self)
			{
				return;
			}
			Self->bEntityIdFetchInFlight = false;

			if (bSuccess && Response.Json.IsValid())
			{
				const TArray<TSharedPtr<FJsonValue>>* Tables;
				if (Response.Json->TryGetArrayField(TEXT("data"), Tables))
				{
					for (const TSharedPtr<FJsonValue>& TableValue : *Tables)
					{
						const TSharedPtr<FJsonObject>* Table;
						if (TableValue->TryGetObject(Table))
						{
							Self->EntityIdCache.Add((*Table)->GetStringField(TEXT("name")), (*Table)->GetStringField(TEXT("entity_id")));
						}
					}
				}
			}

			TArray<TPair<FString, TFunction<void(const FString&, const FPlayServError&)>>> Pending = MoveTemp(Self->PendingResolutions);
			Self->PendingResolutions.Reset();
			for (TPair<FString, TFunction<void(const FString&, const FPlayServError&)>>& Entry : Pending)
			{
				if (const FString* Resolved = Self->EntityIdCache.Find(Entry.Key))
				{
					Entry.Value(*Resolved, FPlayServError::Success());
				}
				else if (!bSuccess)
				{
					Entry.Value(FString(), Error);
				}
				else
				{
					Entry.Value(FString(), FPlayServError::Make(EPlayServErrorCode::NotFound,
						FString::Printf(TEXT("Entity '%s' has no table on the platform — create the table (named after the class) before using this entity"), *Entry.Key)));
				}
			}
		}));
}

TSharedPtr<FJsonObject> UPlayServData::KeysetPageBody(const TSharedPtr<FJsonObject>& BaseBody, const FString& AfterId)
{
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	if (BaseBody.IsValid())
	{
		Body->Values = BaseBody->Values;
	}
	Body->RemoveField(TEXT("cursor"));

	TSharedPtr<FJsonObject> ById = MakeShared<FJsonObject>();
	ById->SetStringField(TEXT("field"), TEXT("id"));
	ById->SetStringField(TEXT("dir"), TEXT("asc"));
	TArray<TSharedPtr<FJsonValue>> Sort;
	Sort.Add(MakeShared<FJsonValueObject>(ById));
	Body->SetArrayField(TEXT("sort"), Sort);

	if (!AfterId.IsEmpty())
	{
		// A new array: the base body's filters are shared with every page and stay as they were.
		TArray<TSharedPtr<FJsonValue>> Filters;
		const TArray<TSharedPtr<FJsonValue>>* BaseFilters;
		if (Body->TryGetArrayField(TEXT("filters"), BaseFilters))
		{
			Filters = *BaseFilters;
		}
		TSharedPtr<FJsonObject> After = MakeShared<FJsonObject>();
		After->SetStringField(TEXT("field"), TEXT("id"));
		After->SetStringField(TEXT("op"), TEXT("gt"));
		After->SetStringField(TEXT("value"), AfterId);
		Filters.Add(MakeShared<FJsonValueObject>(After));
		Body->SetArrayField(TEXT("filters"), Filters);
	}
	return Body;
}

void UPlayServData::QueryAllPages(const FString& EntId, const TSharedPtr<FJsonObject>& BaseBody, TSharedPtr<FJsonObject> Accumulator, FPlayServJsonCallback Callback, const FString& AfterId)
{
	TWeakObjectPtr<UPlayServData> WeakThis(this);
	Http->Request(EPlayServHttpVerb::Post, FString::Printf(TEXT("/data/tables/%s/records:query"), *EntId), KeysetPageBody(BaseBody, AfterId), FPlayServV2Callback::CreateLambda(
		[WeakThis, EntId, BaseBody, Accumulator, Callback, AfterId](bool bSuccess, const FPlayServHttpResponse& Response, const FPlayServError& Error)
		{
			UPlayServData* Self = WeakThis.Get();
			if (!Self)
			{
				return;
			}

			if (!bSuccess || !Response.Json.IsValid())
			{
				Callback.ExecuteIfBound(false, nullptr, Error);
				return;
			}

			TArray<TSharedPtr<FJsonValue>> Items = Accumulator->HasField(TEXT("items"))
				? Accumulator->GetArrayField(TEXT("items"))
				: TArray<TSharedPtr<FJsonValue>>();
			const TArray<TSharedPtr<FJsonValue>>* Page;
			FString LastId;
			if (Response.Json->TryGetArrayField(TEXT("data"), Page))
			{
				Items.Append(*Page);
				const TSharedPtr<FJsonObject>* Last;
				if (Page->Num() > 0 && Page->Last().IsValid() && Page->Last()->TryGetObject(Last))
				{
					(*Last)->TryGetStringField(TEXT("id"), LastId);
				}
			}
			Accumulator->SetArrayField(TEXT("items"), Items);

			const TSharedPtr<FJsonObject>* PageInfo;
			bool bHasMore = false;
			if (Response.Json->TryGetObjectField(TEXT("page"), PageInfo))
			{
				(*PageInfo)->TryGetBoolField(TEXT("has_more"), bHasMore);
			}

			if (bHasMore)
			{
				// A page that names no last record, or the one the page before ended on, cannot be followed. The read fails
				// rather than come back short: a caller takes a short read for the whole table.
				if (LastId.IsEmpty() || LastId == AfterId)
				{
					Callback.ExecuteIfBound(false, nullptr, FPlayServError::Make(EPlayServErrorCode::Unknown,
						FString::Printf(TEXT("Reading table '%s' stopped: a page said more records follow, but it did not end on a new record id"), *EntId)));
					return;
				}
				Self->QueryAllPages(EntId, BaseBody, Accumulator, Callback, LastId);
				return;
			}

			Callback.ExecuteIfBound(true, Accumulator, FPlayServError::Success());
		}));
}

void UPlayServData::Get(const FString& EntityType, const FString& Id, FPlayServJsonCallback Callback)
{
	TWeakObjectPtr<UPlayServData> WeakThis(this);
	ResolveEntityId(EntityType, [WeakThis, Id, Callback](const FString& EntId, const FPlayServError& ResolveError)
	{
		UPlayServData* Self = WeakThis.Get();
		if (!Self)
		{
			return;
		}
		if (EntId.IsEmpty())
		{
			Callback.ExecuteIfBound(false, nullptr, ResolveError);
			return;
		}

		Self->Http->Request(EPlayServHttpVerb::Get, FString::Printf(TEXT("/data/tables/%s/records/%s"), *EntId, *Id), nullptr, FPlayServV2Callback::CreateLambda(
			[WeakThis, Id, Callback](bool bSuccess, const FPlayServHttpResponse& Response, const FPlayServError& Error)
			{
				if (UPlayServData* Inner = WeakThis.Get(); Inner != nullptr && bSuccess && !Response.ETag.IsEmpty())
				{
					Inner->ETagsByRecordId.Add(Id, Response.ETag);
				}
				Callback.ExecuteIfBound(bSuccess, Response.Json, Error);
			}));
	});
}

void UPlayServData::GetAll(const FString& EntityType, const FPlayServFilter& Filter, FPlayServJsonCallback Callback)
{
	GetAll(EntityType, TArray<FString>(), Filter, MoveTemp(Callback));
}

void UPlayServData::GetAll(const FString& EntityType, const TArray<FString>& Ids, FPlayServJsonCallback Callback)
{
	GetAll(EntityType, Ids, FPlayServFilter::None(), MoveTemp(Callback));
}

void UPlayServData::GetAll(const FString& EntityType, const TArray<FString>& Ids, const FPlayServFilter& Filter, FPlayServJsonCallback Callback)
{
	TWeakObjectPtr<UPlayServData> WeakThis(this);
	TArray<FString> IdsCopy = Ids;
	FPlayServFilter FilterCopy = Filter;
	ResolveEntityId(EntityType, [WeakThis, IdsCopy, FilterCopy, Callback](const FString& EntId, const FPlayServError& ResolveError)
	{
		UPlayServData* Self = WeakThis.Get();
		if (!Self)
		{
			return;
		}
		if (EntId.IsEmpty())
		{
			Callback.ExecuteIfBound(false, nullptr, ResolveError);
			return;
		}

		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Filters = FilterCopy.ToV2FiltersArray();
		if (IdsCopy.Num() > 0)
		{
			TArray<TSharedPtr<FJsonValue>> IdValues;
			for (const FString& Id : IdsCopy)
			{
				IdValues.Add(MakeShared<FJsonValueString>(Id));
			}
			TSharedPtr<FJsonObject> IdCondition = MakeShared<FJsonObject>();
			IdCondition->SetStringField(TEXT("field"), TEXT("id"));
			IdCondition->SetStringField(TEXT("op"), TEXT("in"));
			IdCondition->SetArrayField(TEXT("value"), IdValues);
			Filters.Add(MakeShared<FJsonValueObject>(IdCondition));
		}
		if (Filters.Num() > 0)
		{
			Body->SetArrayField(TEXT("filters"), Filters);
		}
		Body->SetNumberField(TEXT("limit"), 200);

		if (IdsCopy.Num() > 0)
		{
			Self->QueryRecordsByIds(EntId, IdsCopy, Body, Callback);
			return;
		}

		Self->QueryAllPages(EntId, Body, MakeShared<FJsonObject>(), Callback);
	});
}

void UPlayServData::QueryRecordsByIds(const FString& EntId, const TArray<FString>& Ids, const TSharedPtr<FJsonObject>& Body, FPlayServJsonCallback Callback)
{
	TArray<FString> IdsCopy = Ids;
	QueryAllPages(EntId, Body, MakeShared<FJsonObject>(), FPlayServJsonCallback::CreateLambda(
		[IdsCopy, Callback](bool bSuccess, const TSharedPtr<FJsonObject>& Result, const FPlayServError& Error)
		{
			if (!bSuccess || !Result.IsValid())
			{
				Callback.ExecuteIfBound(false, nullptr, Error);
				return;
			}

			TMap<FString, TSharedPtr<FJsonValue>> RowsByRecordId;
			const TArray<TSharedPtr<FJsonValue>>* Rows;
			if (Result->TryGetArrayField(TEXT("items"), Rows))
			{
				for (const TSharedPtr<FJsonValue>& Row : *Rows)
				{
					const TSharedPtr<FJsonObject>* RowObject;
					FString RecordId;
					if (Row.IsValid() && Row->TryGetObject(RowObject) && (*RowObject)->TryGetStringField(TEXT("id"), RecordId))
					{
						RowsByRecordId.Add(RecordId, Row);
					}
				}
			}

			TArray<TSharedPtr<FJsonValue>> Ordered;
			for (const FString& Id : IdsCopy)
			{
				if (const TSharedPtr<FJsonValue>* Row = RowsByRecordId.Find(Id))
				{
					Ordered.Add(*Row);
				}
				else
				{
					UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ Data: id batch — %s not found, skipped"), *Id);
				}
			}

			TSharedPtr<FJsonObject> Reordered = MakeShared<FJsonObject>();
			Reordered->SetArrayField(TEXT("items"), Ordered);
			Callback.ExecuteIfBound(true, Reordered, FPlayServError::Success());
		}));
}

void UPlayServData::Delete(const FString& EntityType, const FString& Id, FPlayServSimpleCallback Callback)
{
	TWeakObjectPtr<UPlayServData> WeakThis(this);
	ResolveEntityId(EntityType, [WeakThis, Id, Callback](const FString& EntId, const FPlayServError& ResolveError)
	{
		UPlayServData* Self = WeakThis.Get();
		if (!Self)
		{
			return;
		}
		if (EntId.IsEmpty())
		{
			Callback.ExecuteIfBound(false, ResolveError);
			return;
		}

		TArray<FPlayServHeader> Headers;
		if (const FString* ETag = Self->ETagsByRecordId.Find(Id))
		{
			Headers.Emplace(TEXT("If-Match"), *ETag);
		}

		Self->Http->Request(EPlayServHttpVerb::Delete, FString::Printf(TEXT("/data/tables/%s/records/%s"), *EntId, *Id), nullptr, FPlayServV2Callback::CreateLambda(
			[WeakThis, Id, Callback](bool bSuccess, const FPlayServHttpResponse&, const FPlayServError& Error)
			{
				if (UPlayServData* Inner = WeakThis.Get(); Inner != nullptr && bSuccess)
				{
					Inner->ETagsByRecordId.Remove(Id);
				}
				Callback.ExecuteIfBound(bSuccess, Error);
			}), Headers);
	});
}

void UPlayServData::DeleteAll(const FString& EntityType, const FPlayServFilter& Filter, FPlayServDeleteAllCallback Callback)
{
	if (Filter.IsEmpty() && !Filter.IsAll())
	{
		UE_LOG(LogPlayServ, Warning, TEXT("PlayServ Data: DeleteAll %s rejected — empty filter without All() marker"), *EntityType);
		Callback.ExecuteIfBound(false, FPlayServDeleteAllResult(), FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("PSDeleteAll requires an explicit filter or FPlayServFilter::All(). Use All() to confirm full deletion.")));
		return;
	}

	UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ Data: DeleteAll %s (filter=%s)"),
		*EntityType, Filter.IsAll() ? TEXT("ALL") : TEXT("present"));

	TWeakObjectPtr<UPlayServData> WeakThis(this);
	const FString EntityTypeCopy = EntityType;
	GetAll(EntityType, Filter, FPlayServJsonCallback::CreateLambda(
		[WeakThis, EntityTypeCopy, Callback](bool bSuccess, const TSharedPtr<FJsonObject>& Result, const FPlayServError& Error)
		{
			UPlayServData* Self = WeakThis.Get();
			if (!Self)
			{
				return;
			}
			if (!bSuccess || !Result.IsValid())
			{
				Callback.ExecuteIfBound(false, FPlayServDeleteAllResult(), Error);
				return;
			}

			TArray<FString> Ids;
			const TArray<TSharedPtr<FJsonValue>>* Items;
			if (Result->TryGetArrayField(TEXT("items"), Items))
			{
				for (const TSharedPtr<FJsonValue>& ItemValue : *Items)
				{
					const TSharedPtr<FJsonObject>* Item;
					if (ItemValue->TryGetObject(Item))
					{
						Ids.Add((*Item)->GetStringField(TEXT("id")));
					}
				}
			}
			Self->DeleteAll(EntityTypeCopy, Ids, Callback);
		}));
}

void UPlayServData::DeleteAll(const FString& EntityType, const TArray<FString>& Ids, FPlayServDeleteAllCallback Callback)
{
	UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ Data: DeleteAll %s (ids count=%d)"), *EntityType, Ids.Num());

	if (Ids.Num() == 0)
	{
		Callback.ExecuteIfBound(true, FPlayServDeleteAllResult(), FPlayServError::Success());
		return;
	}

	struct FDeleteAllState
	{
		int32 Remaining = 0;
		int32 Deleted = 0;
		bool bAnyFailed = false;
		FPlayServError FirstError;
	};
	TSharedPtr<FDeleteAllState> State = MakeShared<FDeleteAllState>();
	State->Remaining = Ids.Num();

	for (const FString& Id : Ids)
	{
		Delete(EntityType, Id, FPlayServSimpleCallback::CreateLambda(
			[State, Callback](bool bSuccess, const FPlayServError& Error)
			{
				if (bSuccess)
				{
					++State->Deleted;
				}
				else if (!State->bAnyFailed)
				{
					State->bAnyFailed = true;
					State->FirstError = Error;
				}

				if (--State->Remaining == 0)
				{
					FPlayServDeleteAllResult Payload;
					Payload.DeletedCount = State->Deleted;
					Callback.ExecuteIfBound(!State->bAnyFailed, Payload, State->bAnyFailed ? State->FirstError : FPlayServError::Success());
				}
			}));
	}
}

void UPlayServData::LoadEntity(
	UObject* Outer,
	UClass* Class,
	const FString& EntityType,
	const FString& Id,
	FPlayServEntityLoadCallback Callback)
{
	UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ Data: LoadEntity %s/%s"), *EntityType, *Id);

	TWeakObjectPtr<UObject> WeakOuter(Outer);

	Get(EntityType, Id, FPlayServJsonCallback::CreateLambda(
		[WeakOuter, Class, Id, Callback](bool bSuccess, const TSharedPtr<FJsonObject>& Result, const FPlayServError& Error)
		{
			if (!bSuccess)
			{
				Callback.ExecuteIfBound(false, nullptr, Error);
				return;
			}

			UObject* OuterObj = WeakOuter.Get();
			if (!OuterObj)
			{
				Callback.ExecuteIfBound(false, nullptr, FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("Outer object was garbage collected")));
				return;
			}

			if (!Result.IsValid())
			{
				Callback.ExecuteIfBound(false, nullptr, FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("Server returned empty response")));
				return;
			}

			UObject* Entity = NewObject<UObject>(OuterObj, Class);
			FPlayServRecordIds::Bind(Entity, Id);

			const TSharedPtr<FJsonObject>* PayloadObj;
			if (Result->TryGetObjectField(TEXT("payload"), PayloadObj))
			{
				FPlayServSerializer::JsonToUObject(*PayloadObj, Entity);
			}
			else
			{
				FPlayServSerializer::JsonToUObject(Result, Entity);
			}

			FPlayServChangeTracker::TakeSnapshot(Entity);
			Callback.ExecuteIfBound(true, Entity, FPlayServError::Success());
		}));
}

void UPlayServData::LoadSingletonEntity(UObject* Outer, UClass* Class, FPlayServEntityLoadCallback Callback)
{
	if (!IsSingletonClass(Class))
	{
		Callback.ExecuteIfBound(false, nullptr, FPlayServError::Make(EPlayServErrorCode::Unknown, FString::Printf(TEXT("'%s' is not a singleton class: mark it UCLASS(PlayServSingleton)"), Class != nullptr ? *Class->GetName() : TEXT("<null>"))));
		return;
	}

	UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ Data: LoadSingletonEntity %s"), *Class->GetName());

	TWeakObjectPtr<UObject> WeakOuter(Outer);
	GetSingleton(Class->GetName(), FPlayServJsonCallback::CreateLambda(
		[WeakOuter, Class, Callback](bool bSuccess, const TSharedPtr<FJsonObject>& Result, const FPlayServError& Error)
		{
			if (!bSuccess)
			{
				Callback.ExecuteIfBound(false, nullptr, Error);
				return;
			}

			UObject* OuterObj = WeakOuter.Get();
			if (!OuterObj)
			{
				Callback.ExecuteIfBound(false, nullptr, FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("Outer object was garbage collected")));
				return;
			}

			if (!Result.IsValid())
			{
				Callback.ExecuteIfBound(false, nullptr, FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("Server returned empty response")));
				return;
			}

			UObject* Entity = NewObject<UObject>(OuterObj, Class);
			ApplyFetchedJson(Entity, Result);
			Callback.ExecuteIfBound(true, Entity, FPlayServError::Success());
		}));
}

void UPlayServData::LoadPlayerOwnedEntity(UObject* Outer, UClass* Class, const APlayerController* Player, FPlayServEntityLoadPlayerOwnedCallback Callback)
{
	UPlayServSubsystem* Subsystem = UPlayServSubsystem::Get();
	UPlayServAuth* Auth = Subsystem != nullptr ? Subsystem->GetAuth() : nullptr;
	if (Player == nullptr)
	{
		Callback.ExecuteIfBound(false, nullptr, false, FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("LoadPlayerOwned needs a player controller; it is null.")));
		return;
	}
	if (Auth == nullptr || !Auth->IsLoggedIn())
	{
		Callback.ExecuteIfBound(false, nullptr, false, FPlayServError::Make(EPlayServErrorCode::Unauthorized, TEXT("LoadPlayerOwned needs a session: log in first.")));
		return;
	}

	FString PlayerId;
	if (Auth->IsServerSession())
	{
		UPlayServRooms* Rooms = Subsystem->GetRooms();
		PlayerId = Rooms != nullptr ? Rooms->GetPlayerId(Player) : FString();
		if (PlayerId.IsEmpty())
		{
			Callback.ExecuteIfBound(false, nullptr, false, FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("This connection has no PlayServ-verified player: a server knows the player of a connection only once PlayServ::Rooms::VerifyTicket admitted it.")));
			return;
		}
	}
	else
	{
		if (!Player->IsLocalController())
		{
			Callback.ExecuteIfBound(false, nullptr, false, FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("On a client, LoadPlayerOwned takes the local player's controller.")));
			return;
		}
		PlayerId = Auth->GetPlayerId();
	}

	FetchPlayerOwned(Outer, Class, PlayerId, MoveTemp(Callback));
}

void UPlayServData::LoadPlayerOwnedEntity(UObject* Outer, UClass* Class, const FString& PlayerId, FPlayServEntityLoadPlayerOwnedCallback Callback)
{
	UPlayServSubsystem* Subsystem = UPlayServSubsystem::Get();
	UPlayServAuth* Auth = Subsystem != nullptr ? Subsystem->GetAuth() : nullptr;
	if (Auth == nullptr || !Auth->IsLoggedIn())
	{
		Callback.ExecuteIfBound(false, nullptr, false, FPlayServError::Make(EPlayServErrorCode::Unauthorized, TEXT("LoadPlayerOwned needs a session: log in first.")));
		return;
	}
	if (PlayerId.IsEmpty())
	{
		Callback.ExecuteIfBound(false, nullptr, false, FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("LoadPlayerOwned needs a player id; it is empty.")));
		return;
	}
	if (!Auth->IsServerSession() && PlayerId != Auth->GetPlayerId())
	{
		Callback.ExecuteIfBound(false, nullptr, false, FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("On a client, LoadPlayerOwned loads only the signed-in player's row.")));
		return;
	}

	FetchPlayerOwned(Outer, Class, PlayerId, MoveTemp(Callback));
}

void UPlayServData::LoadAllEntities(
	UObject* Outer,
	UClass* Class,
	const FString& EntityType,
	const FPlayServFilter& Filter,
	FPlayServEntityLoadAllCallback Callback)
{
	UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ Data: LoadAllEntities %s (filter=%s)"),
		*EntityType, Filter.IsEmpty() ? TEXT("none") : TEXT("present"));

	TWeakObjectPtr<UObject> WeakOuter(Outer);
	TWeakObjectPtr<UPlayServData> WeakThis(this);

	GetAll(EntityType, Filter, FPlayServJsonCallback::CreateLambda(
		[WeakThis, WeakOuter, Class, EntityType, Callback](bool bSuccess, const TSharedPtr<FJsonObject>& Result, const FPlayServError& Error)
		{
			if (!bSuccess)
			{
				Callback.ExecuteIfBound(false, TArray<UObject*>(), Error);
				return;
			}

			UObject* OuterObj = WeakOuter.Get();
			if (!OuterObj)
			{
				Callback.ExecuteIfBound(false, TArray<UObject*>(), FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("Outer object was garbage collected")));
				return;
			}

			if (!Result.IsValid())
			{
				Callback.ExecuteIfBound(false, TArray<UObject*>(), FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("Server returned empty response")));
				return;
			}

			const TArray<TSharedPtr<FJsonValue>>* Items;
			if (!Result->TryGetArrayField(TEXT("items"), Items))
			{
				UE_LOG(LogPlayServ, Warning, TEXT("PlayServ Data: LoadAllEntities %s — malformed response: missing 'items' field"), *EntityType);
				Callback.ExecuteIfBound(false, TArray<UObject*>(), FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("Malformed response: missing 'items' field")));
				return;
			}

			TArray<UObject*> Entities;
			for (const TSharedPtr<FJsonValue>& Item : *Items)
			{
				const TSharedPtr<FJsonObject>* ItemObj;
				if (!Item->TryGetObject(ItemObj))
				{
					continue;
				}

				UObject* Entity = NewObject<UObject>(OuterObj, Class);

				FString ItemId;
				if ((*ItemObj)->TryGetStringField(TEXT("id"), ItemId))
				{
					FPlayServRecordIds::Bind(Entity, ItemId);
					if (UPlayServData* Self = WeakThis.Get())
					{
						Self->RememberVersionOf(ItemId, *ItemObj);
					}
				}

				const TSharedPtr<FJsonObject>* PayloadObj;
				if ((*ItemObj)->TryGetObjectField(TEXT("payload"), PayloadObj))
				{
					FPlayServSerializer::JsonToUObject(*PayloadObj, Entity);
				}
				else
				{
					FPlayServSerializer::JsonToUObject(*ItemObj, Entity);
				}

				FPlayServChangeTracker::TakeSnapshot(Entity);
				Entities.Add(Entity);
			}

			Callback.ExecuteIfBound(true, Entities, FPlayServError::Success());
		}));
}

void UPlayServData::EnqueueOp(UObject* Entity, EEntityOpKind Kind, FPlayServSimpleCallback Callback)
{
	if (!Http.IsValid())
	{
		Callback.ExecuteIfBound(false, FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("PlayServ data module is unavailable.")));
		return;
	}

	const FObjectKey Key(Entity);
	FEntityOpQueue& Queue = EntityOpQueues.FindOrAdd(Key);
	Queue.Entity = Entity;

	if (Queue.Pending.Num() > 0 && Queue.Pending.Last().Kind == Kind)
	{
		Queue.Pending.Last().Callbacks.Add(MoveTemp(Callback));
	}
	else
	{
		FPendingEntityOp NewOp;
		NewOp.Kind = Kind;
		NewOp.Callbacks.Add(MoveTemp(Callback));
		Queue.Pending.Add(MoveTemp(NewOp));
	}

	if (!Queue.bBusy)
	{
		// Warning: RunNextOp may run the op synchronously and reallocate EntityOpQueues; Queue must not be used after it.
		RunNextOp(Key);
	}
}

void UPlayServData::RunNextOp(const FObjectKey& Key)
{
	FEntityOpQueue* Queue = EntityOpQueues.Find(Key);
	if (Queue == nullptr || Queue->Pending.Num() == 0)
	{
		if (Queue != nullptr)
		{
			EntityOpQueues.Remove(Key);
		}
		return;
	}

	Queue->bBusy = true;
	FPendingEntityOp Op = MoveTemp(Queue->Pending[0]);
	Queue->Pending.RemoveAt(0);
	UObject* Entity = Queue->Entity.Get();
	// Warning: the dispatch below may run synchronously and reallocate EntityOpQueues; Queue must not be used past this line.

	if (Entity == nullptr)
	{
		for (FPlayServSimpleCallback& Cb : Op.Callbacks)
		{
			Cb.ExecuteIfBound(false, FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("Entity was garbage collected before its queued operation ran.")));
		}
		FinishOp(Key, false);
		return;
	}

	const EEntityOpKind Kind = Op.Kind;
	TSharedPtr<TArray<FPlayServSimpleCallback>> Callbacks = MakeShared<TArray<FPlayServSimpleCallback>>(MoveTemp(Op.Callbacks));
	TWeakObjectPtr<UPlayServData> WeakThis(this);

	FPlayServSimpleCallback Completion = FPlayServSimpleCallback::CreateLambda(
		[WeakThis, Key, Callbacks, Kind](bool bSuccess, const FPlayServError& Error)
		{
			UPlayServData* Self = WeakThis.Get();
			if (Self == nullptr)
			{
				return;
			}

			const bool bTerminalDelete = (Kind == EEntityOpKind::Delete) && bSuccess;

			for (FPlayServSimpleCallback& Cb : *Callbacks)
			{
				Cb.ExecuteIfBound(bSuccess, Error);
			}

			Self->FinishOp(Key, bTerminalDelete);
		});

	switch (Kind)
	{
	case EEntityOpKind::Save:
		RunSaveEntity(Entity, MoveTemp(Completion));
		break;
	case EEntityOpKind::Reload:
		RunReloadEntity(Entity, MoveTemp(Completion));
		break;
	case EEntityOpKind::Delete:
		RunDeleteEntity(Entity, MoveTemp(Completion));
		break;
	}
}

void UPlayServData::FinishOp(const FObjectKey& Key, bool bTerminalDelete)
{
	FEntityOpQueue* Queue = EntityOpQueues.Find(Key);
	if (Queue == nullptr)
	{
		return;
	}

	Queue->bBusy = false;

	if (bTerminalDelete)
	{
		TArray<FPendingEntityOp> Drained = MoveTemp(Queue->Pending);
		EntityOpQueues.Remove(Key);
		for (FPendingEntityOp& Op : Drained)
		{
			for (FPlayServSimpleCallback& Cb : Op.Callbacks)
			{
				Cb.ExecuteIfBound(false, FPlayServError::Make(EPlayServErrorCode::NotFound, TEXT("Entity was deleted; queued operation cancelled.")));
			}
		}
		return;
	}

	if (Queue->Pending.Num() > 0)
	{
		RunNextOp(Key);
	}
	else
	{
		EntityOpQueues.Remove(Key);
	}
}

void UPlayServData::SaveEntity(UObject* Entity, FPlayServSimpleCallback Callback)
{
	if (!Entity)
	{
		UE_LOG(LogPlayServ, Warning, TEXT("PlayServ Data: SaveEntity rejected — entity is null"));
		Callback.ExecuteIfBound(false, FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("Entity is null.")));
		return;
	}

	UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ Data: SaveEntity %s/%s (enqueue)"),
		*Entity->GetClass()->GetName(), *FPlayServRecordIds::Get(Entity));

	EnqueueOp(Entity, EEntityOpKind::Save, MoveTemp(Callback));
}

void UPlayServData::RunSaveEntity(UObject* Entity, FPlayServSimpleCallback Completion)
{
	if (IsSingletonClass(Entity->GetClass()) && !FPlayServChangeTracker::HasSnapshot(Entity))
	{
		Completion.ExecuteIfBound(false, FPlayServError::Make(EPlayServErrorCode::Unknown, FString::Printf(TEXT("'%s' is a singleton and this instance was never read from the platform, so saving it would overwrite the row with class defaults. Get it with PlayServ::Data::LoadSingleton, or Reload it, first."), *Entity->GetClass()->GetName())));
		return;
	}
	if (IsPlayerOwnedClass(Entity->GetClass()) && FPlayServRecordIds::Get(Entity).IsEmpty())
	{
		Completion.ExecuteIfBound(false, FPlayServError::Make(EPlayServErrorCode::Unknown, FString::Printf(TEXT("'%s' is player-owned and this instance is not a player's row. Get the row with PlayServ::Data::LoadPlayerOwned."), *Entity->GetClass()->GetName())));
		return;
	}

	TArray<FPlayServPendingWrite> Writes;
	FPlayServChangeTracker::CollectDirtyEntities(Entity, Writes);

	if (Writes.Num() == 0)
	{
		Completion.ExecuteIfBound(true, FPlayServError::Success());
		return;
	}

#if !UE_BUILD_SHIPPING
	++TestDispatchedUpsertCount;
#endif

	RunPendingWrite(Writes[0], MoveTemp(Completion));
}

void UPlayServData::RunPendingWrite(const FPlayServPendingWrite& Write, FPlayServSimpleCallback Completion)
{
	TWeakObjectPtr<UPlayServData> WeakThis(this);
	const FPlayServPendingWrite WriteCopy = Write;

	ResolveEntityId(Write.EntityType, [WeakThis, WriteCopy, Completion](const FString& EntId, const FPlayServError& ResolveError)
	{
		UPlayServData* Self = WeakThis.Get();
		if (!Self)
		{
			return;
		}
		if (EntId.IsEmpty())
		{
			Completion.ExecuteIfBound(false, ResolveError);
			return;
		}

		if (WriteCopy.bSingleton)
		{
			const FString VersionKey = SingletonVersionKey(WriteCopy.EntityType);
			TArray<FPlayServHeader> SingletonHeaders;
			if (const FString* ETag = Self->ETagsByRecordId.Find(VersionKey))
			{
				SingletonHeaders.Emplace(TEXT("If-Match"), *ETag);
			}

			Self->Http->Request(EPlayServHttpVerb::Patch, FString::Printf(TEXT("/data/tables/%s/singleton"), *EntId), WriteCopy.Body, FPlayServV2Callback::CreateLambda(
				[WeakThis, WriteCopy, VersionKey, Completion](bool bSuccess, const FPlayServHttpResponse& Response, const FPlayServError& Error)
				{
					if (bSuccess)
					{
						if (UObject* Obj = WriteCopy.SourceObject.Get(); Obj != nullptr && WriteCopy.CurrentJson.IsValid())
						{
							FPlayServChangeTracker::TakeSnapshot(Obj, WriteCopy.CurrentJson);
						}
						if (UPlayServData* Inner = WeakThis.Get())
						{
							Inner->RememberResponseVersion(VersionKey, Response.ETag, Response.Json);
						}
					}
					Completion.ExecuteIfBound(bSuccess, Error);
				}), SingletonHeaders);
			return;
		}

		if (WriteCopy.bIsCreate)
		{
			Self->Http->Request(EPlayServHttpVerb::Post, FString::Printf(TEXT("/data/tables/%s/records"), *EntId), WriteCopy.Body, FPlayServV2Callback::CreateLambda(
				[WeakThis, WriteCopy, Completion](bool bSuccess, const FPlayServHttpResponse& Response, const FPlayServError& Error)
				{
					UPlayServData* Inner = WeakThis.Get();
					if (!bSuccess)
					{
						Completion.ExecuteIfBound(false, Error);
						return;
					}

					UObject* Obj = WriteCopy.SourceObject.Get();
					FString NewId;
					if (Response.Json.IsValid())
					{
						Response.Json->TryGetStringField(TEXT("id"), NewId);
					}
					if (NewId.IsEmpty())
					{
						Completion.ExecuteIfBound(false, FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("Create succeeded but the response carried no record id")));
						return;
					}

					if (Obj != nullptr)
					{
						FPlayServRecordIds::Bind(Obj, NewId);
						if (WriteCopy.CurrentJson.IsValid())
						{
							FPlayServChangeTracker::TakeSnapshot(Obj, WriteCopy.CurrentJson);
						}
					}
					if (Inner != nullptr)
					{
						Inner->RememberResponseVersion(NewId, Response.ETag, Response.Json);
					}
					Completion.ExecuteIfBound(true, FPlayServError::Success());
				}));
			return;
		}

		TArray<FPlayServHeader> Headers;
		if (const FString* ETag = Self->ETagsByRecordId.Find(WriteCopy.Id))
		{
			Headers.Emplace(TEXT("If-Match"), *ETag);
		}

		Self->Http->Request(EPlayServHttpVerb::Patch, FString::Printf(TEXT("/data/tables/%s/records/%s"), *EntId, *WriteCopy.Id), WriteCopy.Body, FPlayServV2Callback::CreateLambda(
			[WeakThis, WriteCopy, Completion](bool bSuccess, const FPlayServHttpResponse& Response, const FPlayServError& Error)
			{
				UPlayServData* Inner = WeakThis.Get();
				if (bSuccess)
				{
					if (UObject* Obj = WriteCopy.SourceObject.Get(); Obj != nullptr && WriteCopy.CurrentJson.IsValid())
					{
						FPlayServChangeTracker::TakeSnapshot(Obj, WriteCopy.CurrentJson);
					}
					if (Inner != nullptr)
					{
						Inner->RememberResponseVersion(WriteCopy.Id, Response.ETag, Response.Json);
					}
				}
				Completion.ExecuteIfBound(bSuccess, Error);
			}), Headers);
	});
}

void UPlayServData::ReloadEntity(UObject* Entity, FPlayServSimpleCallback Callback)
{
	if (!Entity)
	{
		UE_LOG(LogPlayServ, Warning, TEXT("PlayServ Data: ReloadEntity rejected — entity is null"));
		Callback.ExecuteIfBound(false, FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("Entity is null.")));
		return;
	}

	UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ Data: ReloadEntity %s/%s (enqueue)"), *Entity->GetClass()->GetName(), *FPlayServRecordIds::Get(Entity));

	EnqueueOp(Entity, EEntityOpKind::Reload, MoveTemp(Callback));
}

void UPlayServData::RunReloadEntity(UObject* Entity, FPlayServSimpleCallback Completion)
{
	const FString EntityType = Entity->GetClass()->GetName();
	TWeakObjectPtr<UObject> WeakEntity(Entity);
	TWeakObjectPtr<UPlayServData> WeakThis(this);
	const FPlayServJsonCallback Apply = FPlayServJsonCallback::CreateLambda(
		[WeakThis, WeakEntity, Completion](bool bSuccess, const TSharedPtr<FJsonObject>& Result, const FPlayServError& Error)
		{
			UObject* Ent = WeakEntity.Get();
			if (!Ent)
			{
				Completion.ExecuteIfBound(false, FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("Entity was garbage collected during reload.")));
				return;
			}

			if (!bSuccess)
			{
				Completion.ExecuteIfBound(false, Error);
				return;
			}

			if (!Result.IsValid())
			{
				Completion.ExecuteIfBound(false, FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("Server returned empty response")));
				return;
			}

			TUniquePtr<FAppliedChange> Change = ApplyBackendDocument(Ent, Result);
			if (UPlayServData* Self = WeakThis.Get())
			{
				Self->DispatchChange(*Change, Self->HandlesOf(Ent));
			}
			Completion.ExecuteIfBound(true, FPlayServError::Success());
		});

	if (IsSingletonClass(Entity->GetClass()))
	{
		UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ Data: ReloadEntity %s singleton (run)"), *EntityType);
		GetSingleton(EntityType, Apply);
		return;
	}

	const FString Id = FPlayServRecordIds::Get(Entity);
	if (Id.IsEmpty())
	{
		UE_LOG(LogPlayServ, Warning, TEXT("PlayServ Data: ReloadEntity failed — entity has no ID at run time"));
		Completion.ExecuteIfBound(false, FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("Entity has no ID. Cannot reload an entity that was never saved or loaded.")));
		return;
	}

	UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ Data: ReloadEntity %s/%s (run)"), *EntityType, *Id);
	Get(EntityType, Id, Apply);
}

void UPlayServData::DeleteSelfEntity(UObject* Entity, FPlayServSimpleCallback Callback)
{
	if (!Entity)
	{
		UE_LOG(LogPlayServ, Warning, TEXT("PlayServ Data: DeleteSelfEntity rejected — entity is null"));
		Callback.ExecuteIfBound(false, FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("Entity is null.")));
		return;
	}
	if (IsSingletonClass(Entity->GetClass()))
	{
		UE_LOG(LogPlayServ, Warning, TEXT("PlayServ Data: DeleteSelfEntity rejected — %s is a singleton"), *Entity->GetClass()->GetName());
		Callback.ExecuteIfBound(false, FPlayServError::Make(EPlayServErrorCode::Unknown, FString::Printf(TEXT("'%s' is a singleton: its row belongs to the table and is never deleted."), *Entity->GetClass()->GetName())));
		return;
	}

	UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ Data: DeleteSelfEntity %s/%s (enqueue)"), *Entity->GetClass()->GetName(), *FPlayServRecordIds::Get(Entity));

	EnqueueOp(Entity, EEntityOpKind::Delete, MoveTemp(Callback));
}

void UPlayServData::RunDeleteEntity(UObject* Entity, FPlayServSimpleCallback Completion)
{
	const FString EntityType = Entity->GetClass()->GetName();
	const FString Id = FPlayServRecordIds::Get(Entity);
	if (Id.IsEmpty())
	{
		UE_LOG(LogPlayServ, Warning, TEXT("PlayServ Data: DeleteSelfEntity failed — entity has no ID at run time"));
		Completion.ExecuteIfBound(false, FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("Entity has no ID. Cannot delete an entity that was never saved.")));
		return;
	}

	UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ Data: DeleteSelfEntity %s/%s (run)"), *EntityType, *Id);

	TWeakObjectPtr<UObject> WeakEntity(Entity);
	Delete(EntityType, Id, FPlayServSimpleCallback::CreateLambda(
		[WeakEntity, Completion](bool bSuccess, const FPlayServError& Error)
		{
			if (bSuccess)
			{
				UObject* Ent = WeakEntity.Get();
				if (Ent)
				{
					FPlayServChangeTracker::ClearSnapshot(Ent);
					FPlayServRecordIds::Forget(Ent);
				}
			}

			Completion.ExecuteIfBound(bSuccess, Error);
		}));
}

void UPlayServData::PopulateArrayProperty(UObject* Entity, const FString& PropertyName, FPlayServSimpleCallback Callback)
{
	FProperty* Prop = Entity->GetClass()->FindPropertyByName(*PropertyName);
	FArrayProperty* ArrayProp = CastField<FArrayProperty>(Prop);
	if (!ArrayProp)
	{
		UE_LOG(LogPlayServ, Warning, TEXT("PlayServ Data: PopulateArrayProperty rejected — property '%s' is not an array"), *PropertyName);
		Callback.ExecuteIfBound(false, FPlayServError::Make(EPlayServErrorCode::Unknown, FString::Printf(TEXT("Property '%s' is not an array"), *PropertyName)));
		return;
	}

	FObjectProperty* InnerObj = CastField<FObjectProperty>(ArrayProp->Inner);
	if (!InnerObj || !FPlayServSerializer::IsPersistentClass(InnerObj->PropertyClass))
	{
		UE_LOG(LogPlayServ, Warning, TEXT("PlayServ Data: PopulateArrayProperty rejected — property '%s' is not an entity reference array"), *PropertyName);
		Callback.ExecuteIfBound(false, FPlayServError::Make(EPlayServErrorCode::Unknown, FString::Printf(TEXT("Property '%s' is not an entity reference array"), *PropertyName)));
		return;
	}

	const void* ArrayPtr = ArrayProp->ContainerPtrToValuePtr<void>(Entity);
	FScriptArrayHelper ArrayHelper(ArrayProp, ArrayPtr);

	int32 Total = ArrayHelper.Num();
	if (Total == 0)
	{
		Callback.ExecuteIfBound(true, FPlayServError::Success());
		return;
	}

	FString EntityType = InnerObj->PropertyClass->GetName();

	UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ Data: PopulateArrayProperty %s.%s (entityType=%s, count=%d)"),
		*Entity->GetClass()->GetName(), *PropertyName, *EntityType, Total);

	TArray<FString> Ids;
	TSet<FString> SeenIds;
	TMap<FString, TWeakObjectPtr<UObject>> IdToElem;

	for (int32 i = 0; i < Total; ++i)
	{
		UObject* Elem = InnerObj->GetObjectPropertyValue(ArrayHelper.GetRawPtr(i));
		if (!Elem || FPlayServRecordIds::Get(Elem).IsEmpty())
		{
			continue;
		}

		FString ElemId = FPlayServRecordIds::Get(Elem);
		if (SeenIds.Contains(ElemId))
		{
			continue;
		}
		SeenIds.Add(ElemId);
		Ids.Add(ElemId);
		IdToElem.Add(ElemId, TWeakObjectPtr<UObject>(Elem));
	}

	if (Ids.Num() == 0)
	{
		Callback.ExecuteIfBound(true, FPlayServError::Success());
		return;
	}

	TSharedPtr<TMap<FString, TWeakObjectPtr<UObject>>> SharedMap = MakeShared<TMap<FString, TWeakObjectPtr<UObject>>>(MoveTemp(IdToElem));
	TWeakObjectPtr<UPlayServData> WeakThis(this);

	GetAll(EntityType, Ids, FPlayServJsonCallback::CreateLambda(
		[WeakThis, SharedMap, Callback](bool bSuccess, const TSharedPtr<FJsonObject>& Result, const FPlayServError& Error)
		{
			if (!bSuccess)
			{
				Callback.ExecuteIfBound(false, Error);
				return;
			}

			const TArray<TSharedPtr<FJsonValue>>* Items;
			if (!Result.IsValid() || !Result->TryGetArrayField(TEXT("items"), Items))
			{
				Callback.ExecuteIfBound(false, FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("PopulateArrayProperty: missing 'items' in response")));
				return;
			}

			UPlayServData* Self = WeakThis.Get();
			TArray<TUniquePtr<FAppliedChange>> Changes;
			for (const TSharedPtr<FJsonValue>& Item : *Items)
			{
				const TSharedPtr<FJsonObject>* ItemObj;
				if (!Item->TryGetObject(ItemObj))
				{
					continue;
				}

				FString ItemId;
				if (!(*ItemObj)->TryGetStringField(TEXT("id"), ItemId))
				{
					continue;
				}

				TWeakObjectPtr<UObject>* WeakElem = SharedMap->Find(ItemId);
				if (!WeakElem)
				{
					continue;
				}

				UObject* ElemObj = WeakElem->Get();
				if (!ElemObj)
				{
					continue;
				}
				if (Self != nullptr)
				{
					Self->RememberVersionOf(ItemId, *ItemObj);
				}
				Changes.Add(ApplyBackendDocument(ElemObj, *ItemObj));
			}

			for (const TUniquePtr<FAppliedChange>& Change : Changes)
			{
				UObject* ElemObj = Change->Instance.Get();
				if (Self != nullptr && ElemObj != nullptr)
				{
					Self->DispatchChange(*Change, Self->HandlesOf(ElemObj));
				}
			}

			Callback.ExecuteIfBound(true, FPlayServError::Success());
		}));
}

void UPlayServData::SaveAllEntities(const TArray<UObject*>& Entities, FPlayServSimpleCallback Callback)
{
	UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ Data: SaveAllEntities (count=%d)"), Entities.Num());

	TSet<const UObject*> Seen;
	TArray<FPlayServPendingWrite> Writes;
	for (const UObject* Entity : Entities)
	{
		if (!Entity || !FPlayServSerializer::IsPersistentClass(Entity->GetClass()) || Seen.Contains(Entity))
		{
			continue;
		}
		if (IsSingletonClass(Entity->GetClass()) && !FPlayServChangeTracker::HasSnapshot(Entity))
		{
			Callback.ExecuteIfBound(false, FPlayServError::Make(EPlayServErrorCode::Unknown, FString::Printf(TEXT("'%s' is a singleton and this instance was never read from the platform; nothing was saved. Get it with PlayServ::Data::LoadSingleton, or Reload it, first."), *Entity->GetClass()->GetName())));
			return;
		}
		if (IsPlayerOwnedClass(Entity->GetClass()) && FPlayServRecordIds::Get(Entity).IsEmpty())
		{
			Callback.ExecuteIfBound(false, FPlayServError::Make(EPlayServErrorCode::Unknown, FString::Printf(TEXT("'%s' is player-owned and this instance is not a player's row; nothing was saved. Get the row with PlayServ::Data::LoadPlayerOwned."), *Entity->GetClass()->GetName())));
			return;
		}
		Seen.Add(Entity);
		FPlayServChangeTracker::CollectDirtyEntities(Entity, Writes);
	}

	if (Writes.Num() == 0)
	{
		Callback.ExecuteIfBound(true, FPlayServError::Success());
		return;
	}

#if !UE_BUILD_SHIPPING
	++TestDispatchedUpsertCount;
#endif

	TSharedRef<FPlayServBatchCounter> Counter = MakeShared<FPlayServBatchCounter>(Writes.Num(), MoveTemp(Callback));
	for (const FPlayServPendingWrite& Write : Writes)
	{
		RunPendingWrite(Write, FPlayServSimpleCallback::CreateLambda(
			[Counter](bool bSuccess, const FPlayServError& Error)
			{
				Counter->Complete(bSuccess, Error);
			}));
	}
}

void UPlayServData::DeleteEntities(const TArray<UObject*>& Entities, FPlayServSimpleCallback Callback)
{
	if (Entities.Num() == 0)
	{
		Callback.ExecuteIfBound(true, FPlayServError::Success());
		return;
	}

	UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ Data: DeleteEntities (count=%d)"), Entities.Num());

	TSharedRef<FPlayServBatchCounter> Counter = MakeShared<FPlayServBatchCounter>(Entities.Num(), MoveTemp(Callback));

	for (UObject* Entity : Entities)
	{
		if (!Entity || !FPlayServSerializer::IsPersistentClass(Entity->GetClass()))
		{
			Counter->Complete(true, FPlayServError::Success());
			continue;
		}

		FString EntityType = Entity->GetClass()->GetName();
		FString RecordId = FPlayServRecordIds::Get(Entity);

		if (IsSingletonClass(Entity->GetClass()))
		{
			Counter->Complete(false, FPlayServError::Make(EPlayServErrorCode::Unknown, FString::Printf(TEXT("'%s' is a singleton: its row belongs to the table and is never deleted."), *EntityType)));
			continue;
		}

		if (RecordId.IsEmpty())
		{
			UE_LOG(LogPlayServ, Warning, TEXT("PlayServ Data: DeleteEntities — %s has no ID, cannot delete"), *EntityType);
			Counter->Complete(false, FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("Entity has no ID. Cannot delete an entity that was never saved.")));
			continue;
		}

		Delete(EntityType, RecordId, FPlayServSimpleCallback::CreateLambda(
			[Counter](bool bSuccess, const FPlayServError& Error)
			{
				Counter->Complete(bSuccess, Error);
			}));
	}
}

void UPlayServData::RunSchemaAdvisoryCheck()
{
	static bool bAdvisoryRan = false;
	if (bAdvisoryRan || !Http.IsValid())
	{
		return;
	}
	bAdvisoryRan = true;

	Http->Request(EPlayServHttpVerb::Get, TEXT("/data/tables"), nullptr, FPlayServV2Callback::CreateLambda(
		[](bool bSuccess, const FPlayServHttpResponse& Response, const FPlayServError& Error)
		{
			if (!bSuccess || !Response.Json.IsValid())
			{
				UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ: schema advisory skipped — tables fetch failed (%s)"), *Error.Message);
				return;
			}

			const TArray<TSharedPtr<FJsonValue>>* Tables;
			if (!Response.Json->TryGetArrayField(TEXT("data"), Tables))
			{
				return;
			}

			TSet<FString> PlatformTables;
			TSet<FString> PlatformSingletons;
			for (const TSharedPtr<FJsonValue>& TableValue : *Tables)
			{
				const TSharedPtr<FJsonObject>* Table;
				if (TableValue->TryGetObject(Table))
				{
					const FString TableName = (*Table)->GetStringField(TEXT("name"));
					PlatformTables.Add(TableName);
					bool bSingleton = false;
					if ((*Table)->TryGetBoolField(TEXT("singleton"), bSingleton) && bSingleton)
					{
						PlatformSingletons.Add(TableName);
					}
				}
			}

			TArray<FString> ClassPaths;
			FPlayServCodegenRegistry::Get().GetRegisteredClassPaths(ClassPaths);

			TSet<FString> Excluded;
			{
				FString OverlayContent;
				if (FFileHelper::LoadFileToString(OverlayContent, *(FPaths::ProjectConfigDir() / TEXT("PlayServSchemaOverlay.json"))))
				{
					TSharedPtr<FJsonObject> Overlay;
					TSharedRef<TJsonReader<>> OverlayReader = TJsonReaderFactory<>::Create(OverlayContent);
					const TArray<TSharedPtr<FJsonValue>>* ExcludedArr;
					if (FJsonSerializer::Deserialize(OverlayReader, Overlay) && Overlay.IsValid()
						&& Overlay->TryGetArrayField(TEXT("excludedClassPaths"), ExcludedArr))
					{
						for (const TSharedPtr<FJsonValue>& Value : *ExcludedArr)
						{
							Excluded.Add(Value->AsString());
						}
					}
				}
			}

			int32 MissingCount = 0;
			int32 CheckedCount = 0;
			for (const FString& ClassPath : ClassPaths)
			{
				if (Excluded.Contains(ClassPath))
				{
					continue;
				}
				++CheckedCount;
				const FString ClassName = SchemaNameForClassPath(ClassPath);

				if (!PlatformTables.Contains(ClassName))
				{
					++MissingCount;
					UE_LOG(LogPlayServ, Warning, TEXT("PlayServ: schema advisory — entity class '%s' has NO table on the platform (name '%s'). Create the table or its reads/writes will 404."), *ClassPath, *ClassName);
				}
				else
				{
					PlatformTables.Remove(ClassName);
					const FPlayServClassDescriptor* Descriptor = FPlayServCodegenRegistry::Get().FindForClassPath(ClassPath);
					const bool bClassSingleton = Descriptor != nullptr && Descriptor->bSingleton;
					if (bClassSingleton != PlatformSingletons.Contains(ClassName))
					{
						UE_LOG(LogPlayServ, Warning, TEXT("PlayServ: schema advisory — entity class '%s' is %s, but its platform table '%s' is %s. Its reads and writes will fail until the two agree."), *ClassPath, bClassSingleton ? TEXT("marked PlayServSingleton") : TEXT("not a singleton"), *ClassName, bClassSingleton ? TEXT("not a singleton") : TEXT("a singleton"));
					}
				}
			}

			for (const FString& Orphan : PlatformTables)
			{
				UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ: schema advisory — platform table '%s' has no local entity class in this build"), *Orphan);
			}

			if (MissingCount == 0)
			{
				UE_LOG(LogPlayServ, Display, TEXT("PlayServ: schema advisory — all %d checked entity classes have platform tables (%d overlay-excluded)"), CheckedCount, ClassPaths.Num() - CheckedCount);
			}
		}));
}

bool UPlayServData::IsSingletonClass(const UClass* Class)
{
	const FPlayServClassDescriptor* Descriptor = FPlayServCodegenRegistry::Get().FindForClass(Class);
	return Descriptor != nullptr && Descriptor->bSingleton;
}

bool UPlayServData::IsPlayerOwnedClass(const UClass* Class)
{
	const FPlayServClassDescriptor* Descriptor = FPlayServCodegenRegistry::Get().FindForClass(Class);
	return Descriptor != nullptr && Descriptor->bPlayerOwned;
}

static const TCHAR* const PlayerIdField = TEXT("player_id");

struct UPlayServData::FPlayerRowRequest
{
	TWeakObjectPtr<UObject> Outer;
	UClass* Class = nullptr;
	FString EntityType;
	FString EntId;
	FString PlayerId;
	bool bServerSession = false;
	FPlayServEntityLoadPlayerOwnedCallback Callback;
};

void UPlayServData::FetchPlayerOwned(UObject* Outer, UClass* Class, const FString& PlayerId, FPlayServEntityLoadPlayerOwnedCallback Callback)
{
	if (!IsPlayerOwnedClass(Class))
	{
		Callback.ExecuteIfBound(false, nullptr, false, FPlayServError::Make(EPlayServErrorCode::Unknown, FString::Printf(TEXT("'%s' is not a player-owned class: mark it UCLASS(PlayServEntity, PlayServPlayerOwned)."), Class != nullptr ? *Class->GetName() : TEXT("<null>"))));
		return;
	}
	if (!Http.IsValid())
	{
		Callback.ExecuteIfBound(false, nullptr, false, FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("PlayServ data module is unavailable.")));
		return;
	}

	TSharedRef<FPlayerRowRequest> Request = MakeShared<FPlayerRowRequest>();
	Request->Outer = Outer;
	Request->Class = Class;
	Request->EntityType = Class->GetName();
	Request->PlayerId = PlayerId;
	UPlayServSubsystem* Subsystem = UPlayServSubsystem::Get();
	const UPlayServAuth* Auth = Subsystem != nullptr ? Subsystem->GetAuth() : nullptr;
	Request->bServerSession = Auth != nullptr && Auth->IsServerSession();
	Request->Callback = MoveTemp(Callback);

	UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ Data: LoadPlayerOwned %s"), *Request->EntityType);

	TWeakObjectPtr<UPlayServData> WeakThis(this);
	ResolveEntityId(Request->EntityType, [WeakThis, Request](const FString& EntId, const FPlayServError& ResolveError)
	{
		UPlayServData* Self = WeakThis.Get();
		if (!Self)
		{
			return;
		}
		if (EntId.IsEmpty())
		{
			Request->Callback.ExecuteIfBound(false, nullptr, false, ResolveError);
			return;
		}
		Request->EntId = EntId;

		const FString Path = FString::Printf(TEXT("/data/tables/%s/records:by-natural-key?field=%s&value=%s"), *EntId, PlayerIdField, *FGenericPlatformHttp::UrlEncode(Request->PlayerId));
		Self->Http->Request(EPlayServHttpVerb::Get, Path, nullptr, FPlayServV2Callback::CreateLambda(
			[WeakThis, Request](bool bSuccess, const FPlayServHttpResponse& Response, const FPlayServError& Error)
			{
				UPlayServData* Inner = WeakThis.Get();
				if (!Inner)
				{
					return;
				}
				if (bSuccess)
				{
					Inner->AdoptPlayerRow(Request, Response.Json, Response.ETag, false);
					return;
				}
				if (Error.ProblemCode == TEXT("field_not_unique"))
				{
					FPlayServError MissingField = Error;
					MissingField.Message = FString::Printf(TEXT("The %s table holds player-owned rows, so it needs a unique, required text field named %s: %s"), *Request->EntityType, PlayerIdField, *Error.Message);
					Request->Callback.ExecuteIfBound(false, nullptr, false, MissingField);
					return;
				}
				if (Error.Code != EPlayServErrorCode::NotFound)
				{
					Request->Callback.ExecuteIfBound(false, nullptr, false, Error);
					return;
				}
				if (Request->bServerSession)
				{
					Request->Callback.ExecuteIfBound(false, nullptr, false, FPlayServError::Make(EPlayServErrorCode::NotFound, FString::Printf(TEXT("%s has no row for this player yet. A dedicated server does not create player-owned rows; the player's side creates it."), *Request->EntityType)));
					return;
				}
				Inner->CreatePlayerRow(Request);
			}));
	});
}

void UPlayServData::CreatePlayerRow(const TSharedRef<FPlayerRowRequest>& Request)
{
	TSharedPtr<FJsonObject> Record = FPlayServSerializer::UObjectToJson(Request->Class->GetDefaultObject());
	if (!Record.IsValid())
	{
		Record = MakeShared<FJsonObject>();
	}
	Record->SetStringField(PlayerIdField, Request->PlayerId);

	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("field"), PlayerIdField);
	Body->SetStringField(TEXT("value"), Request->PlayerId);
	Body->SetStringField(TEXT("mode"), TEXT("seed"));
	Body->SetObjectField(TEXT("record"), Record);

	TWeakObjectPtr<UPlayServData> WeakThis(this);
	Http->Request(EPlayServHttpVerb::Post, FString::Printf(TEXT("/data/tables/%s/records:upsert"), *Request->EntId), Body, FPlayServV2Callback::CreateLambda(
		[WeakThis, Request](bool bSuccess, const FPlayServHttpResponse& Response, const FPlayServError& Error)
		{
			UPlayServData* Self = WeakThis.Get();
			if (!Self)
			{
				return;
			}
			if (!bSuccess)
			{
				if (Error.Code == EPlayServErrorCode::Forbidden || Error.ProblemCode == TEXT("acting_player_required"))
				{
					FPlayServError NotProvisioned = Error;
					NotProvisioned.Code = EPlayServErrorCode::NotFound;
					NotProvisioned.Message = FString::Printf(TEXT("%s has no row for this player, and this session may not create one: %s"), *Request->EntityType, *Error.Message);
					Request->Callback.ExecuteIfBound(false, nullptr, false, NotProvisioned);
					return;
				}
				Request->Callback.ExecuteIfBound(false, nullptr, false, Error);
				return;
			}

			FString RecordId;
			bool bCreated = false;
			if (Response.Json.IsValid())
			{
				Response.Json->TryGetStringField(TEXT("id"), RecordId);
				Response.Json->TryGetBoolField(TEXT("created"), bCreated);
			}
			if (RecordId.IsEmpty())
			{
				Request->Callback.ExecuteIfBound(false, nullptr, false, FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("The platform created the row but its answer carried no record id.")));
				return;
			}

			Self->Http->Request(EPlayServHttpVerb::Get, FString::Printf(TEXT("/data/tables/%s/records/%s"), *Request->EntId, *RecordId), nullptr, FPlayServV2Callback::CreateLambda(
				[WeakThis, Request, bCreated](bool bReadOk, const FPlayServHttpResponse& Read, const FPlayServError& ReadError)
				{
					UPlayServData* Inner = WeakThis.Get();
					if (!Inner)
					{
						return;
					}
					if (!bReadOk)
					{
						Request->Callback.ExecuteIfBound(false, nullptr, false, ReadError);
						return;
					}
					Inner->AdoptPlayerRow(Request, Read.Json, Read.ETag, bCreated);
				}));
		}));
}

void UPlayServData::AdoptPlayerRow(const TSharedRef<FPlayerRowRequest>& Request, const TSharedPtr<FJsonObject>& Row, const FString& ETag, bool bCreated)
{
	UObject* OuterObj = Request->Outer.Get();
	if (!OuterObj)
	{
		Request->Callback.ExecuteIfBound(false, nullptr, false, FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("Outer object was garbage collected")));
		return;
	}

	FString RecordId;
	if (!Row.IsValid() || !Row->TryGetStringField(TEXT("id"), RecordId) || RecordId.IsEmpty())
	{
		Request->Callback.ExecuteIfBound(false, nullptr, false, FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("The platform's row carried no record id.")));
		return;
	}

	FString Owner;
	if (Row->TryGetStringField(TEXT("owner"), Owner) && !Owner.IsEmpty() && Owner != Request->PlayerId)
	{
		UE_LOG(LogPlayServ, Warning, TEXT("PlayServ Data: LoadPlayerOwned refused — the %s row %s is keyed to this player but owned by another"), *Request->EntityType, *RecordId);
		Request->Callback.ExecuteIfBound(false, nullptr, false, FPlayServError::Make(EPlayServErrorCode::Unknown, FString::Printf(TEXT("The %s row keyed to this player is owned by another player, so it was not loaded."), *Request->EntityType)));
		return;
	}

	UObject* Entity = NewObject<UObject>(OuterObj, Request->Class);
	FPlayServRecordIds::Bind(Entity, RecordId);
	RememberResponseVersion(RecordId, ETag, Row);
	ApplyFetchedJson(Entity, Row);
	Request->Callback.ExecuteIfBound(true, Entity, bCreated, FPlayServError::Success());
}

FString UPlayServData::SingletonVersionKey(const FString& EntityType)
{
	return EntityType + TEXT("/singleton");
}

void UPlayServData::GetSingleton(const FString& EntityType, FPlayServJsonCallback Callback)
{
	TWeakObjectPtr<UPlayServData> WeakThis(this);
	ResolveEntityId(EntityType, [WeakThis, EntityType, Callback](const FString& EntId, const FPlayServError& ResolveError)
	{
		UPlayServData* Self = WeakThis.Get();
		if (!Self)
		{
			return;
		}
		if (EntId.IsEmpty())
		{
			Callback.ExecuteIfBound(false, nullptr, ResolveError);
			return;
		}

		Self->Http->Request(EPlayServHttpVerb::Get, FString::Printf(TEXT("/data/tables/%s/singleton"), *EntId), nullptr, FPlayServV2Callback::CreateLambda(
			[WeakThis, EntityType, Callback](bool bSuccess, const FPlayServHttpResponse& Response, const FPlayServError& Error)
			{
				if (UPlayServData* Inner = WeakThis.Get(); Inner != nullptr && bSuccess)
				{
					Inner->RememberResponseVersion(SingletonVersionKey(EntityType), Response.ETag, Response.Json);
				}
				Callback.ExecuteIfBound(bSuccess, Response.Json, Error);
			}));
	});
}

void UPlayServData::RememberResponseVersion(const FString& VersionKey, const FString& ETag, const TSharedPtr<FJsonObject>& Json)
{
	if (VersionKey.IsEmpty())
	{
		return;
	}
	if (!ETag.IsEmpty())
	{
		ETagsByRecordId.Add(VersionKey, ETag);
		return;
	}
	RememberVersionOf(VersionKey, Json);
}

FString UPlayServData::SchemaNameForClassPath(const FString& ClassPath)
{
	int32 DotIdx;
	return ClassPath.FindLastChar(TEXT('.'), DotIdx) ? ClassPath.RightChop(DotIdx + 1) : ClassPath;
}

FString UPlayServData::ETagForUpdatedAt(const FString& UpdatedAt)
{
	// Warning: FDateTime::ParseIso8601 rounds a fraction longer than three digits; the platform's tag truncates it.
	FString Truncated = UpdatedAt;
	int32 DotIdx;
	if (Truncated.FindChar(TEXT('.'), DotIdx))
	{
		int32 End = DotIdx + 1;
		while (End < Truncated.Len() && FChar::IsDigit(Truncated[End]))
		{
			++End;
		}
		if (End - (DotIdx + 1) > 3)
		{
			Truncated = Truncated.Left(DotIdx + 4) + Truncated.Mid(End);
		}
	}

	FDateTime Parsed;
	if (Truncated.IsEmpty() || !FDateTime::ParseIso8601(*Truncated, Parsed))
	{
		return FString();
	}
	const int64 Ticks = Parsed.GetTicks();
	const int64 MillisecondTicks = Ticks - Ticks % ETimespan::TicksPerMillisecond;
	return FString::Printf(TEXT("\"%llx\""), static_cast<unsigned long long>(MillisecondTicks));
}

bool UPlayServData::RememberVersionOf(const FString& RecordId, const TSharedPtr<FJsonObject>& Record)
{
	FString UpdatedAt;
	if (RecordId.IsEmpty() || !Record.IsValid() || !Record->TryGetStringField(TEXT("updated_at"), UpdatedAt))
	{
		return false;
	}
	const FString Tag = ETagForUpdatedAt(UpdatedAt);
	if (Tag.IsEmpty())
	{
		return false;
	}
	ETagsByRecordId.Add(RecordId, Tag);
	return true;
}

FPlayServSubscriptionHandle UPlayServData::SubscribeEntity(UObject* Entity, FOnPlayServObjectChanged Delegate)
{
	if (Entity == nullptr)
	{
		UE_LOG(LogPlayServ, Warning, TEXT("PlayServ Data: Subscribe rejected — null entity"));
		return FPlayServSubscriptionHandle();
	}
	if (!IsRegisteredPersistentClass(Entity->GetClass()))
	{
		UE_LOG(LogPlayServ, Warning, TEXT("PlayServ Data: Subscribe rejected — %s is not a persistent entity class"), *Entity->GetClass()->GetName());
		return FPlayServSubscriptionHandle();
	}
	if (IsSingletonClass(Entity->GetClass()))
	{
		UE_LOG(LogPlayServ, Warning, TEXT("PlayServ Data: Subscribe rejected — %s is a singleton, and changes to a singleton are not delivered to subscribers yet; Reload it instead"), *Entity->GetClass()->GetName());
		return FPlayServSubscriptionHandle();
	}
	const FString RecordId = FPlayServRecordIds::Get(Entity);
	if (RecordId.IsEmpty())
	{
		UE_LOG(LogPlayServ, Warning, TEXT("PlayServ Data: Subscribe rejected — entity has no server id (save or load it first)"));
		return FPlayServSubscriptionHandle();
	}

	UPlayServSubsystem* Subsystem = UPlayServSubsystem::Get();
	UPlayServAuth* Auth = Subsystem ? Subsystem->GetAuth() : nullptr;
	if (Auth == nullptr || !Auth->IsLoggedIn() || Auth->IsServerSession())
	{
		UE_LOG(LogPlayServ, Warning, TEXT("PlayServ Data: Subscribe rejected — the dataflow plane requires a logged-in CLIENT session"));
		return FPlayServSubscriptionHandle();
	}

	if (!Dataflow.IsValid())
	{
		Dataflow = MakeShared<FPlayServDataflow>();
		TWeakObjectPtr<UPlayServData> WeakThis(this);
		Dataflow->OnUpdate.BindLambda([WeakThis](const FString& EntityType, const FString& InRecordId, TSharedPtr<FJsonObject> Document)
		{
			if (UPlayServData* Self = WeakThis.Get())
			{
				Self->OnDataflowUpdate(EntityType, InRecordId, Document);
			}
		});
		Dataflow->OnTerminated.BindLambda([WeakThis](const FString& EntityType, const FString& InRecordId, int32 Code)
		{
			if (UPlayServData* Self = WeakThis.Get())
			{
				Self->OnDataflowTerminated(EntityType, InRecordId, Code);
			}
		});
		Dataflow->OnConnectionLost.BindLambda([WeakThis]()
		{
			if (UPlayServData* Self = WeakThis.Get())
			{
				Self->OnDataflowConnectionLost();
			}
		});
	}

	const FString EntityType = Entity->GetClass()->GetName();

	FEntitySubscription Subscription;
	Subscription.Entity = Entity;
	Subscription.EntityType = EntityType;
	Subscription.RecordId = RecordId;
	Subscription.Delegate = MoveTemp(Delegate);

	FPlayServSubscriptionHandle Handle;
	Handle.Id = NextSubscriptionHandleId++;
	SubscriptionsByHandle.Add(Handle.Id, MoveTemp(Subscription));

	Dataflow->SubscribeRecord(EntityType, RecordId);
	UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ Data: subscribed %s %s (handle %llu)"), *EntityType, *RecordId, Handle.Id);
	return Handle;
}

void UPlayServData::UnsubscribeEntity(FPlayServSubscriptionHandle Handle)
{
	if (!Handle.IsValid())
	{
		return;
	}
	FEntitySubscription Removed;
	if (!SubscriptionsByHandle.RemoveAndCopyValue(Handle.Id, Removed))
	{
		return;
	}
	if (Dataflow.IsValid())
	{
		Dataflow->UnsubscribeRecord(Removed.EntityType, Removed.RecordId);
	}
	UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ Data: unsubscribed %s %s (handle %llu)"), *Removed.EntityType, *Removed.RecordId, Handle.Id);
}

void UPlayServData::OnDataflowUpdate(const FString& EntityType, const FString& RecordId, TSharedPtr<FJsonObject> Document)
{
	if (!Document.IsValid())
	{
		return;
	}
	const bool bVersionKnown = RememberVersionOf(RecordId, Document);

	TArray<TWeakObjectPtr<UObject>> Instances;
	TArray<TArray<uint64>> HandlesByInstance;
	for (auto It = SubscriptionsByHandle.CreateIterator(); It; ++It)
	{
		const FEntitySubscription& Subscription = It.Value();
		if (Subscription.EntityType != EntityType || Subscription.RecordId != RecordId)
		{
			continue;
		}
		UObject* Entity = Subscription.Entity.Get();
		if (Entity == nullptr)
		{
			It.RemoveCurrent();
			continue;
		}
		int32 Index = Instances.IndexOfByPredicate([Entity](const TWeakObjectPtr<UObject>& Known) { return Known.Get() == Entity; });
		if (Index == INDEX_NONE)
		{
			Index = Instances.Add(Entity);
			HandlesByInstance.AddDefaulted();
		}
		HandlesByInstance[Index].Add(It.Key());
	}

	TArray<TUniquePtr<FAppliedChange>> Changes;
	bool bAnyChanged = false;
	for (const TWeakObjectPtr<UObject>& Instance : Instances)
	{
		UObject* Entity = Instance.Get();
		TUniquePtr<FAppliedChange>& Change = Changes.Add_GetRef(Entity != nullptr ? ApplyBackendDocument(Entity, Document) : nullptr);
		bAnyChanged |= Change.IsValid() && Change->ChangedFields.Num() > 0;
	}

	// Warning: a push carries no updated_at, so a tag held from before it would refuse the next save of a current instance.
	if (!bVersionKnown && bAnyChanged)
	{
		ETagsByRecordId.Remove(RecordId);
	}

	for (int32 Index = 0; Index < Changes.Num(); ++Index)
	{
		if (Changes[Index].IsValid())
		{
			DispatchChange(*Changes[Index], HandlesByInstance[Index]);
		}
	}
}

void UPlayServData::OnDataflowTerminated(const FString& EntityType, const FString& RecordId, int32 Code)
{
	int32 Dropped = 0;
	for (auto It = SubscriptionsByHandle.CreateIterator(); It; ++It)
	{
		if (It.Value().EntityType == EntityType && It.Value().RecordId == RecordId)
		{
			It.RemoveCurrent();
			++Dropped;
		}
	}
	if (Dropped > 0)
	{
		UE_LOG(LogPlayServ, Verbose, TEXT("PlayServ Data: subscription to %s %s terminated by the platform (code %d) — %d handle(s) dropped"), *EntityType, *RecordId, Code, Dropped);
	}
}

void UPlayServData::OnDataflowConnectionLost()
{
	if (SubscriptionsByHandle.Num() > 0)
	{
		UE_LOG(LogPlayServ, Warning, TEXT("PlayServ Data: dataflow connection lost — dropping %d subscription handle(s)"), SubscriptionsByHandle.Num());
		SubscriptionsByHandle.Empty();
	}
}

TUniquePtr<UPlayServData::FAppliedChange> UPlayServData::ApplyBackendDocument(UObject* Instance, const TSharedPtr<FJsonObject>& Document)
{
	TUniquePtr<FAppliedChange> Change = MakeUnique<FAppliedChange>();
	Change->Instance = Instance;
	const bool bHeldBackendData = FPlayServChangeTracker::HasSnapshot(Instance);
	const TSharedPtr<FJsonObject> Before = FPlayServSerializer::UObjectToJson(Instance);

	const FPlayServClassDescriptor* Descriptor = FPlayServCodegenRegistry::Get().FindForClass(Instance->GetClass());
	if (bHeldBackendData && Descriptor != nullptr && Descriptor->HasChangeHooks())
	{
		for (const FPlayServFieldDesc& Field : Descriptor->Fields)
		{
			if (Field.OnChanged == nullptr || Field.Property == nullptr)
			{
				continue;
			}
			FAppliedChange::FFieldCall& Call = Change->Calls.AddDefaulted_GetRef();
			Call.Function = Field.OnChanged;
			Call.Field = Field.Property;
			Call.WireName = Field.WireName;
			if (Field.OnChanged->NumParms > 0)
			{
				Call.OldValue = FMemory::Malloc(Field.Property->GetSize(), Field.Property->GetMinAlignment());
				Field.Property->InitializeValue(Call.OldValue);
				Field.Property->CopyCompleteValue(Call.OldValue, Field.Property->ContainerPtrToValuePtr<void>(Instance));
				if (const FObjectPropertyBase* ObjectField = CastField<FObjectPropertyBase>(Field.Property))
				{
					if (UObject* Previous = ObjectField->GetObjectPropertyValue(Call.OldValue))
					{
						Change->KeepAlive.Emplace(Previous);
					}
				}
			}
		}
	}

	const TSharedPtr<FJsonObject> After = ApplyFetchedJson(Instance, Document);

	TArray<FString> ChangedKeys;
	FPlayServChangeTracker::ChangedTopLevelFields(Before, After, ChangedKeys);
	for (const FString& Key : ChangedKeys)
	{
		Change->ChangedFields.Add(FName(*Key));
	}
	for (int32 Index = Change->Calls.Num() - 1; Index >= 0; --Index)
	{
		if (!ChangedKeys.Contains(Change->Calls[Index].WireName))
		{
			FAppliedChange::Release(Change->Calls[Index]);
			Change->Calls.RemoveAt(Index);
		}
	}
	return Change;
}

void UPlayServData::DispatchChange(const FAppliedChange& Change, const TArray<uint64>& HandleIds)
{
	for (const FAppliedChange::FFieldCall& Call : Change.Calls)
	{
		UObject* Instance = Change.Instance.Get();
		if (!IsValid(Instance))
		{
			return;
		}
		CallFieldFunction(Instance, Call.Function, Call.Field, Call.OldValue);
	}

	if (Change.ChangedFields.Num() == 0)
	{
		return;
	}
	for (const uint64 HandleId : HandleIds)
	{
		UObject* Instance = Change.Instance.Get();
		if (!IsValid(Instance))
		{
			return;
		}
		const FEntitySubscription* Subscription = SubscriptionsByHandle.Find(HandleId);
		if (Subscription == nullptr || Subscription->Entity.Get() != Instance)
		{
			continue;
		}
		const FOnPlayServObjectChanged Delegate = Subscription->Delegate;
		Delegate.ExecuteIfBound(Instance, Change.ChangedFields);
	}
}

TArray<uint64> UPlayServData::HandlesOf(const UObject* Instance) const
{
	TArray<uint64> HandleIds;
	for (const TPair<uint64, FEntitySubscription>& Pair : SubscriptionsByHandle)
	{
		if (Pair.Value.Entity.Get() == Instance)
		{
			HandleIds.Add(Pair.Key);
		}
	}
	HandleIds.Sort();
	return HandleIds;
}

#if !UE_BUILD_SHIPPING

int32 UPlayServData::TestDispatchedUpsertCount = 0;

int32 UPlayServData::TestPendingDepth(const UObject* Entity) const
{
	const FEntityOpQueue* Queue = EntityOpQueues.Find(FObjectKey(Entity));
	return Queue ? Queue->Pending.Num() : 0;
}

void UPlayServData::TestDrainViaShutdown()
{
	TSharedPtr<FPlayServHttp> SavedHttp = Http;
	Shutdown();
	Init(SavedHttp);
}

void UPlayServData::TestSetForceReflectionSerialization(bool bForce)
{
	FPlayServSerializer::bTestForceReflectionWalk = bForce;
}

int32 UPlayServData::TestCodegenClassCount()
{
	return FPlayServCodegenRegistry::Get().NumRegisteredClasses();
}

bool UPlayServData::TestClassHasCodegenDescriptor(const UClass* Class)
{
	return FPlayServCodegenRegistry::Get().HasDescriptorForClass(Class);
}

bool UPlayServData::TestClassClientWritable(const UClass* Class)
{
	const FPlayServClassDescriptor* Descriptor = FPlayServCodegenRegistry::Get().FindForClass(Class);
	return Descriptor != nullptr && Descriptor->bClientWritable;
}

bool UPlayServData::TestClassIsPartial(const UClass* Class)
{
	const FPlayServClassDescriptor* Descriptor = FPlayServCodegenRegistry::Get().FindForClass(Class);
	return Descriptor != nullptr && Descriptor->bPartial;
}

bool UPlayServData::TestFieldClientWritable(const UClass* Class, const FName& FieldName)
{
	const FPlayServClassDescriptor* Descriptor = FPlayServCodegenRegistry::Get().FindForClass(Class);
	if (Descriptor == nullptr)
	{
		return false;
	}
	for (const FPlayServFieldDesc& Field : Descriptor->Fields)
	{
		if (Field.Name == FieldName)
		{
			return Field.bClientWritable;
		}
	}
	return false;
}

int32 UPlayServData::TestDescriptorFieldCount(const UClass* Class)
{
	const FPlayServClassDescriptor* Descriptor = FPlayServCodegenRegistry::Get().FindForClass(Class);
	return Descriptor != nullptr ? Descriptor->Fields.Num() : -1;
}

TSharedPtr<FJsonObject> UPlayServData::TestSerializeToJson(const UObject* Object)
{
	return FPlayServSerializer::UObjectToJson(Object);
}

bool UPlayServData::TestDeserializeFromJson(const TSharedPtr<FJsonObject>& Json, UObject* Object)
{
	return FPlayServSerializer::JsonToUObject(Json, Object);
}

void UPlayServData::TestRegisterStructMemberNames(const TCHAR* StructPath, const TCHAR* const* FieldNames, int32 FieldCount)
{
	// Warning: the registry keeps the path pointer until its next query, so it must outlive the caller's string.
	static TArray<TUniquePtr<FString>> StablePaths;
	const FString& Stable = *StablePaths.Add_GetRef(MakeUnique<FString>(StructPath));
	FPlayServCodegenRegistry::Get().RegisterStruct(*Stable, FieldNames, FieldCount);
}

bool UPlayServData::TestHasStructMemberNames(const UStruct* Struct)
{
	if (!Struct)
	{
		return false;
	}
	for (TFieldIterator<FProperty> It(Struct, EFieldIteratorFlags::ExcludeSuper); It; ++It)
	{
		if (FPlayServCodegenRegistry::Get().FindStructMemberName(*It) == nullptr)
		{
			return false;
		}
	}
	return true;
}

FString UPlayServData::TestVersionTagFor(const FString& RecordId) const
{
	const FString* Found = ETagsByRecordId.Find(RecordId);
	return Found ? *Found : FString();
}

void UPlayServData::TestBindRecordId(const UObject* Object, const FString& RecordId)
{
	FPlayServRecordIds::Bind(Object, RecordId);
}
#endif

bool UPlayServData::HasCodegenDescriptor(const UClass* Class)
{
	return FPlayServCodegenRegistry::Get().HasDescriptorForClass(Class);
}

bool UPlayServData::IsRegisteredPersistentClass(const UClass* Class)
{
	if (!Class)
	{
		return false;
	}

	return HasCodegenDescriptor(Class);
}

FString UPlayServData::GetRecordId(const UObject* Object)
{
	return FPlayServRecordIds::Get(Object);
}
