#pragma once

#include "CoreMinimal.h"

class FProperty;
class UClass;
class UFunction;

namespace EPlayServClassFlags
{
	inline constexpr uint8 None = 0;
	inline constexpr uint8 ClientWritable = 1 << 0;
	inline constexpr uint8 Partial = 1 << 1;
	inline constexpr uint8 Singleton = 1 << 2;
	inline constexpr uint8 PlayerOwned = 1 << 3;
}

namespace EPlayServFieldFlags
{
	inline constexpr uint8 None = 0;
	inline constexpr uint8 ClientWritable = 1 << 0;
}

struct FPlayServFieldDesc
{
	FName Name;
	// Warning: WireName, not Name or FProperty::GetName(), goes on the wire; outside the editor an FName does not keep its casing.
	const TCHAR* WireName = nullptr;
	const TCHAR* OnChangedName = nullptr;
	bool bClientWritable = false;
	FProperty* Property = nullptr;
	UFunction* OnChanged = nullptr;
};

struct FPlayServClassDescriptor
{
	FString ClassPath;
	uint32 SchemaHash = 0;
	TArray<FPlayServFieldDesc> Fields;
	bool bClientWritable = false;
	bool bPartial = false;
	bool bSingleton = false;
	bool bPlayerOwned = false;
	bool bPropertiesResolved = false;

	bool HasChangeHooks() const;
};

// Warning: game thread only; nothing here is locked.
class FPlayServCodegenRegistry
{
public:
	static FPlayServCodegenRegistry& Get();

	void RegisterClass(const TCHAR* InClassPath, const TCHAR* const* InFieldNames, const uint8* InFieldFlags, const TCHAR* const* InFieldOnChanged, int32 InFieldCount, uint32 InSchemaHash, uint8 InClassFlags);

	const FPlayServClassDescriptor* FindForClass(const UClass* Class);

	const FPlayServClassDescriptor* FindForClassPath(const FString& ClassPath);

	bool HasDescriptorForClass(const UClass* Class);

	int32 NumRegisteredClasses();

	void GetRegisteredClassPaths(TArray<FString>& OutClassPaths);

	void RegisterStruct(const TCHAR* InStructPath, const TCHAR* const* InFieldNames, int32 InFieldCount);

	const TCHAR* FindStructMemberName(const FProperty* Property);

private:
	FPlayServCodegenRegistry() = default;

	void FlushPendingRegistrations();

	static void EnsurePropertiesResolved(FPlayServClassDescriptor& ClassDescriptor, const UClass* Class);

	struct FPendingRegistration
	{
		const TCHAR* ClassPath = nullptr;
		const TCHAR* const* FieldNames = nullptr;
		const uint8* FieldFlags = nullptr;
		const TCHAR* const* FieldOnChanged = nullptr;
		int32 FieldCount = 0;
		uint32 SchemaHash = 0;
		uint8 ClassFlags = 0;
	};

	TArray<FPendingRegistration> PendingRegistrations;
	TMap<FString, TUniquePtr<FPlayServClassDescriptor>> DescriptorsByClassPath;
	TMap<const UClass*, FPlayServClassDescriptor*> ResolvedDescriptorsByClass;
	TSet<const UClass*> ClassesWithoutDescriptor;

	struct FPendingStructRegistration
	{
		const TCHAR* StructPath = nullptr;
		const TCHAR* const* FieldNames = nullptr;
		int32 FieldCount = 0;
	};

	void FlushPendingStructRegistrations();

	TArray<FPendingStructRegistration> PendingStructRegistrations;
	TMap<FString, TArray<const TCHAR*>> StructMemberNamesByPath;
	TMap<const UStruct*, TMap<FName, const TCHAR*>> ResolvedStructMemberNames;
};

struct FPlayServCodegenAutoReg
{
	FPlayServCodegenAutoReg(const TCHAR* InClassPath, const TCHAR* const* InFieldNames, const uint8* InFieldFlags, const TCHAR* const* InFieldOnChanged, int32 InFieldCount, uint32 InSchemaHash, uint8 InClassFlags)
	{
		FPlayServCodegenRegistry::Get().RegisterClass(InClassPath, InFieldNames, InFieldFlags, InFieldOnChanged, InFieldCount, InSchemaHash, InClassFlags);
	}
};

struct FPlayServCodegenStructAutoReg
{
	FPlayServCodegenStructAutoReg(const TCHAR* InStructPath, const TCHAR* const* InFieldNames, int32 InFieldCount)
	{
		FPlayServCodegenRegistry::Get().RegisterStruct(InStructPath, InFieldNames, InFieldCount);
	}
};
