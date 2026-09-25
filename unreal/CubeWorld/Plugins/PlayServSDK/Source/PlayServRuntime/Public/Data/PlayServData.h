#pragma once

#include "CoreMinimal.h"
#include "Core/PlayServTypes.h"
#include "Dom/JsonObject.h"
#include "UObject/ObjectKey.h"
#include "UObject/WeakObjectPtr.h"
#include "PlayServData.generated.h"

class APlayerController;
class FPlayServFilter;

/** The callback of Get and GetAll: the records as JSON. */
DECLARE_DELEGATE_ThreeParams(FPlayServJsonCallback, bool /*bSuccess*/, const TSharedPtr<FJsonObject>& /*Result*/, const FPlayServError& /*Error*/);

/** The callback of LoadEntity and LoadSingletonEntity: the loaded entity. */
DECLARE_DELEGATE_ThreeParams(FPlayServEntityLoadCallback, bool /*bSuccess*/, UObject* /*Result*/, const FPlayServError& /*Error*/);

/** The callback of LoadPlayerOwnedEntity: the player's row, and whether this call created it. */
DECLARE_DELEGATE_FourParams(FPlayServEntityLoadPlayerOwnedCallback, bool /*bSuccess*/, UObject* /*Result*/, bool /*bCreated*/, const FPlayServError& /*Error*/);

/** The callback of LoadAllEntities: the loaded entities. */
DECLARE_DELEGATE_ThreeParams(FPlayServEntityLoadAllCallback, bool /*bSuccess*/, const TArray<UObject*>& /*Results*/, const FPlayServError& /*Error*/);

/**
 * Called after the platform's latest state overwrote a subscribed entity, with the names of the changed top-level
 * properties; a change inside a struct or an array names the property that holds it.
 */
DECLARE_DELEGATE_TwoParams(FOnPlayServObjectChanged, UObject* /*Entity*/, const TArray<FName>& /*ChangedFields*/);

/** What Subscribe returns and Unsubscribe takes. A default handle is invalid, and Unsubscribe ignores it. */
struct FPlayServSubscriptionHandle
{
	uint64 Id = 0;
	bool IsValid() const { return Id != 0; }
};

/** The outcome of DeleteAll. */
USTRUCT(BlueprintType)
struct PLAYSERVRUNTIME_API FPlayServDeleteAllResult
{
	GENERATED_BODY()

	/** How many records were deleted. */
	UPROPERTY(BlueprintReadOnly, Category="PlayServ")
	int32 DeletedCount = 0;
};

/** The callback of DeleteAll. */
DECLARE_DELEGATE_ThreeParams(FPlayServDeleteAllCallback, bool /*bSuccess*/, const FPlayServDeleteAllResult& /*Result*/, const FPlayServError& /*Error*/);

/**
 * Entities: loading, saving, queries and live updates. Games use the PlayServ::Data namespace (PlayServ.h), which has
 * the typed forms. This class, UPlayServSubsystem::Get()->GetData(), adds Get, GetAll, Delete and DeleteAll, which work
 * on records as JSON by entity type.
 */
UCLASS()
class PLAYSERVRUNTIME_API UPlayServData : public UObject
{
	GENERATED_BODY()

public:
	// ---- Records as JSON ----

	/** Load one record by entity type and record id: id, created_at, updated_at, owner and the fields, all at the top level. */
	void Get(const FString& EntityType, const FString& Id, FPlayServJsonCallback Callback);

	/**
	 * Load the records that match a filter, a list of ids, or both, as {"items": [...]}, every page fetched. With ids,
	 * the records come back in the order asked, and unknown ids are skipped.
	 */
	void GetAll(const FString& EntityType, const FPlayServFilter& Filter, FPlayServJsonCallback Callback);
	void GetAll(const FString& EntityType, const TArray<FString>& Ids, FPlayServJsonCallback Callback);
	void GetAll(const FString& EntityType, const TArray<FString>& Ids, const FPlayServFilter& Filter, FPlayServJsonCallback Callback);

	/** Delete one record by entity type and record id. */
	void Delete(const FString& EntityType, const FString& Id, FPlayServSimpleCallback Callback);

	/** Delete the records that match a filter, or those with these ids. Deleting every record takes FPlayServFilter::All(). */
	void DeleteAll(const FString& EntityType, const FPlayServFilter& Filter, FPlayServDeleteAllCallback Callback);
	void DeleteAll(const FString& EntityType, const TArray<FString>& Ids, FPlayServDeleteAllCallback Callback);

	// ---- Entities ----

	/** Load one entity by record id; see PlayServ::Data::Load. */
	void LoadEntity(UObject* Outer, UClass* Class, const FString& EntityType, const FString& Id, FPlayServEntityLoadCallback Callback);

	/** Load the one row of a singleton class; see PlayServ::Data::LoadSingleton. */
	void LoadSingletonEntity(UObject* Outer, UClass* Class, FPlayServEntityLoadCallback Callback);

	/** Load this controller's player's row of a player-owned class; see PlayServ::Data::LoadPlayerOwned. */
	void LoadPlayerOwnedEntity(UObject* Outer, UClass* Class, const APlayerController* Player, FPlayServEntityLoadPlayerOwnedCallback Callback);

	/** The same for a player id. */
	void LoadPlayerOwnedEntity(UObject* Outer, UClass* Class, const FString& PlayerId, FPlayServEntityLoadPlayerOwnedCallback Callback);

	/** Load every entity that matches the filter; see PlayServ::Data::LoadAll. */
	void LoadAllEntities(UObject* Outer, UClass* Class, const FString& EntityType, const FPlayServFilter& Filter, FPlayServEntityLoadAllCallback Callback);

	/** See PlayServ::Data::Save. Saves, reloads and deletes of one instance run one at a time, in the order called. */
	void SaveEntity(UObject* Entity, FPlayServSimpleCallback Callback);

	/** See PlayServ::Data::Reload. Queued with the instance's other calls, as SaveEntity is. */
	void ReloadEntity(UObject* Entity, FPlayServSimpleCallback Callback);

	/** See PlayServ::Data::Delete. Queued as SaveEntity is; once it succeeds, the calls queued after it fail with NotFound. */
	void DeleteSelfEntity(UObject* Entity, FPlayServSimpleCallback Callback);

	/** See PlayServ::Data::PopulateArray. */
	void PopulateArrayProperty(UObject* Entity, const FString& PropertyName, FPlayServSimpleCallback Callback);

	/** See PlayServ::Data::BulkSave. */
	void SaveAllEntities(const TArray<UObject*>& Entities, FPlayServSimpleCallback Callback);

	/** See PlayServ::Data::BulkDelete. */
	void DeleteEntities(const TArray<UObject*>& Entities, FPlayServSimpleCallback Callback);

	/** See PlayServ::Data::Subscribe. */
	FPlayServSubscriptionHandle SubscribeEntity(UObject* Entity, FOnPlayServObjectChanged Delegate);

	/** See PlayServ::Data::Unsubscribe. */
	void UnsubscribeEntity(FPlayServSubscriptionHandle Handle);

	/**
	 * Compare the entity classes with the platform's tables and log a warning for each class that has no table, or whose
	 * singleton marking its table does not match. Changes nothing. Runs by itself at the first login of the process.
	 */
	void RunSchemaAdvisoryCheck();

	/** True for an entity class: one marked PlayServEntity or PlayServSingleton. */
	static bool IsRegisteredPersistentClass(const UClass* Class);

	/** The record id the platform minted for this entity, or empty before its first Save. */
	static FString GetRecordId(const UObject* Object);

private:
	friend class UPlayServSubsystem;
	void Init(TSharedPtr<class FPlayServHttp> InHttp);
	void Shutdown();

	void EndRealtimeSession();

private:
	// Warning: game thread only. The op queues and the subscriptions take no lock.
	enum class EEntityOpKind : uint8 { Save, Reload, Delete };

	struct FPendingEntityOp
	{
		EEntityOpKind Kind = EEntityOpKind::Save;
		TArray<FPlayServSimpleCallback> Callbacks;
	};

	struct FEntityOpQueue
	{
		bool bBusy = false;
		TArray<FPendingEntityOp> Pending;
		TWeakObjectPtr<UObject> Entity;
	};

	void EnqueueOp(UObject* Entity, EEntityOpKind Kind, FPlayServSimpleCallback Callback);
	void RunNextOp(const FObjectKey& Key);
	void FinishOp(const FObjectKey& Key, bool bTerminalDelete);

	void RunSaveEntity(UObject* Entity, FPlayServSimpleCallback Completion);
	void RunReloadEntity(UObject* Entity, FPlayServSimpleCallback Completion);
	void RunDeleteEntity(UObject* Entity, FPlayServSimpleCallback Completion);

	void RunPendingWrite(const struct FPlayServPendingWrite& Write, FPlayServSimpleCallback Completion);

	TMap<FObjectKey, FEntityOpQueue> EntityOpQueues;

	static bool HasCodegenDescriptor(const UClass* Class);

	void ResolveEntityId(const FString& EntityType, TFunction<void(const FString& /*EntId*/, const FPlayServError&)> OnResolved);

	TMap<FString, FString> EntityIdCache;
	bool bEntityIdFetchInFlight = false;
	TArray<TPair<FString, TFunction<void(const FString&, const FPlayServError&)>>> PendingResolutions;

	TMap<FString, FString> ETagsByRecordId;

	static bool IsSingletonClass(const UClass* Class);

	static bool IsPlayerOwnedClass(const UClass* Class);

	struct FPlayerRowRequest;
	void FetchPlayerOwned(UObject* Outer, UClass* Class, const FString& PlayerId, FPlayServEntityLoadPlayerOwnedCallback Callback);
	void CreatePlayerRow(const TSharedRef<FPlayerRowRequest>& Request);
	void AdoptPlayerRow(const TSharedRef<FPlayerRowRequest>& Request, const TSharedPtr<FJsonObject>& Row, const FString& ETag, bool bCreated);
	static FString SingletonVersionKey(const FString& EntityType);
	void GetSingleton(const FString& EntityType, FPlayServJsonCallback Callback);
	void RememberResponseVersion(const FString& VersionKey, const FString& ETag, const TSharedPtr<FJsonObject>& Json);

	static FString SchemaNameForClassPath(const FString& ClassPath);
	static FString ETagForUpdatedAt(const FString& UpdatedAt);
	bool RememberVersionOf(const FString& RecordId, const TSharedPtr<FJsonObject>& Record);

	void QueryAllPages(const FString& EntId, const TSharedPtr<FJsonObject>& BaseBody, TSharedPtr<FJsonObject> Accumulator, FPlayServJsonCallback Callback);

	void QueryRecordsByIds(const FString& EntId, const TArray<FString>& Ids, const TSharedPtr<FJsonObject>& Body, FPlayServJsonCallback Callback);

	TSharedPtr<class FPlayServHttp> Http;

	struct FEntitySubscription
	{
		TWeakObjectPtr<UObject> Entity;
		FString EntityType;
		FString RecordId;
		FOnPlayServObjectChanged Delegate;
	};

	TMap<uint64, FEntitySubscription> SubscriptionsByHandle;
	uint64 NextSubscriptionHandleId = 1;
	TSharedPtr<class FPlayServDataflow> Dataflow;

	void OnDataflowUpdate(const FString& EntityType, const FString& RecordId, TSharedPtr<FJsonObject> Document);
	void OnDataflowTerminated(const FString& EntityType, const FString& RecordId, int32 Code);
	void OnDataflowConnectionLost();

	struct FAppliedChange;
	static TUniquePtr<FAppliedChange> ApplyBackendDocument(UObject* Instance, const TSharedPtr<FJsonObject>& Document);
	void DispatchChange(const FAppliedChange& Change, const TArray<uint64>& HandleIds);
	TArray<uint64> HandlesOf(const UObject* Instance) const;

#if !UE_BUILD_SHIPPING
	int32 TestSubscriptionHandleCount() const { return SubscriptionsByHandle.Num(); }
#endif

#if !UE_BUILD_SHIPPING
	friend struct FPlayServDataTestAccess;

	static int32 TestDispatchedUpsertCount;

	int32 TestPendingDepth(const UObject* Entity) const;

	void TestDrainViaShutdown();

	static void TestSetForceReflectionSerialization(bool bForce);
	static int32 TestCodegenClassCount();
	static bool TestClassHasCodegenDescriptor(const UClass* Class);
	static bool TestClassClientWritable(const UClass* Class);
	static bool TestClassIsPartial(const UClass* Class);
	static bool TestFieldClientWritable(const UClass* Class, const FName& FieldName);
	static int32 TestDescriptorFieldCount(const UClass* Class);
	static TSharedPtr<FJsonObject> TestSerializeToJson(const UObject* Object);
	static bool TestDeserializeFromJson(const TSharedPtr<FJsonObject>& Json, UObject* Object);
	static void TestRegisterStructMemberNames(const TCHAR* StructPath, const TCHAR* const* FieldNames, int32 FieldCount);
	static bool TestHasStructMemberNames(const UStruct* Struct);
	FString TestVersionTagFor(const FString& RecordId) const;
	static void TestBindRecordId(const UObject* Object, const FString& RecordId);
#endif
};
