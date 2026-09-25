#include "Data/PlayServCodegenRegistry.h"
#include "Core/PlayServLog.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

FPlayServCodegenRegistry& FPlayServCodegenRegistry::Get()
{
	static FPlayServCodegenRegistry Instance;
	return Instance;
}

bool FPlayServClassDescriptor::HasChangeHooks() const
{
	for (const FPlayServFieldDesc& Field : Fields)
	{
		if (Field.OnChanged != nullptr)
		{
			return true;
		}
	}
	return false;
}

void FPlayServCodegenRegistry::RegisterClass(const TCHAR* InClassPath, const TCHAR* const* InFieldNames, const uint8* InFieldFlags, const TCHAR* const* InFieldOnChanged, int32 InFieldCount, uint32 InSchemaHash, uint8 InClassFlags)
{
	if (!InClassPath)
	{
		return;
	}

	FPendingRegistration& Entry = PendingRegistrations.AddDefaulted_GetRef();
	Entry.ClassPath = InClassPath;
	Entry.FieldNames = InFieldNames;
	Entry.FieldFlags = InFieldFlags;
	Entry.FieldOnChanged = InFieldOnChanged;
	Entry.FieldCount = InFieldNames ? InFieldCount : 0;
	Entry.SchemaHash = InSchemaHash;
	Entry.ClassFlags = InClassFlags;

	ClassesWithoutDescriptor.Empty();
}

void FPlayServCodegenRegistry::FlushPendingRegistrations()
{
	if (PendingRegistrations.Num() == 0)
	{
		return;
	}

	for (const FPendingRegistration& Entry : PendingRegistrations)
	{
		TUniquePtr<FPlayServClassDescriptor> Descriptor = MakeUnique<FPlayServClassDescriptor>();
		Descriptor->ClassPath = Entry.ClassPath;
		Descriptor->SchemaHash = Entry.SchemaHash;
		Descriptor->bClientWritable = (Entry.ClassFlags & EPlayServClassFlags::ClientWritable) != 0;
		Descriptor->bPartial = (Entry.ClassFlags & EPlayServClassFlags::Partial) != 0;
		Descriptor->bSingleton = (Entry.ClassFlags & EPlayServClassFlags::Singleton) != 0;
		Descriptor->bPlayerOwned = (Entry.ClassFlags & EPlayServClassFlags::PlayerOwned) != 0;
		Descriptor->Fields.Reserve(Entry.FieldCount);

		for (int32 i = 0; i < Entry.FieldCount; ++i)
		{
			const uint8 FieldFlags = Entry.FieldFlags != nullptr ? Entry.FieldFlags[i] : EPlayServFieldFlags::None;
			FPlayServFieldDesc& Field = Descriptor->Fields.AddDefaulted_GetRef();
			Field.Name = FName(Entry.FieldNames[i]);
			Field.WireName = Entry.FieldNames[i];
			Field.OnChangedName = Entry.FieldOnChanged != nullptr ? Entry.FieldOnChanged[i] : nullptr;
			Field.bClientWritable = (FieldFlags & EPlayServFieldFlags::ClientWritable) != 0;
		}

		DescriptorsByClassPath.Add(Descriptor->ClassPath, MoveTemp(Descriptor));
	}

	UE_LOG(LogPlayServ, Display, TEXT("PlayServ codegen registry: %d entity class descriptor(s) active"), DescriptorsByClassPath.Num());
	PendingRegistrations.Empty();
}

void FPlayServCodegenRegistry::EnsurePropertiesResolved(FPlayServClassDescriptor& ClassDescriptor, const UClass* Class)
{
	if (ClassDescriptor.bPropertiesResolved)
	{
		return;
	}

	for (FPlayServFieldDesc& Field : ClassDescriptor.Fields)
	{
		Field.Property = Class->FindPropertyByName(Field.Name);
		if (!Field.Property)
		{
			UE_LOG(LogPlayServ, Warning, TEXT("PlayServ codegen: descriptor field '%s' not found on class '%s' — stale generated code?"), *Field.Name.ToString(), *ClassDescriptor.ClassPath);
		}
		if (Field.OnChangedName != nullptr)
		{
			Field.OnChanged = Class->FindFunctionByName(FName(Field.OnChangedName));
			if (!Field.OnChanged)
			{
				UE_LOG(LogPlayServ, Warning, TEXT("PlayServ codegen: change function '%s' of field '%s' not found on class '%s' — stale generated code?"), Field.OnChangedName, *Field.Name.ToString(), *ClassDescriptor.ClassPath);
			}
		}
	}

	ClassDescriptor.bPropertiesResolved = true;
}

const FPlayServClassDescriptor* FPlayServCodegenRegistry::FindForClass(const UClass* Class)
{
	if (!Class)
	{
		return nullptr;
	}

	FlushPendingRegistrations();

	if (const FPlayServClassDescriptor* Cached = ResolvedDescriptorsByClass.FindRef(Class))
	{
		return Cached;
	}
	if (ClassesWithoutDescriptor.Contains(Class))
	{
		return nullptr;
	}

	const TUniquePtr<FPlayServClassDescriptor>* Owned = DescriptorsByClassPath.Find(Class->GetPathName());
	if (!Owned)
	{
		ClassesWithoutDescriptor.Add(Class);
		return nullptr;
	}

	FPlayServClassDescriptor* ClassDescriptor = Owned->Get();
	EnsurePropertiesResolved(*ClassDescriptor, Class);

	ResolvedDescriptorsByClass.Add(Class, ClassDescriptor);
	return ClassDescriptor;
}

const FPlayServClassDescriptor* FPlayServCodegenRegistry::FindForClassPath(const FString& ClassPath)
{
	FlushPendingRegistrations();
	const TUniquePtr<FPlayServClassDescriptor>* Found = DescriptorsByClassPath.Find(ClassPath);
	return Found != nullptr ? Found->Get() : nullptr;
}

bool FPlayServCodegenRegistry::HasDescriptorForClass(const UClass* Class)
{
	return FindForClass(Class) != nullptr;
}

int32 FPlayServCodegenRegistry::NumRegisteredClasses()
{
	FlushPendingRegistrations();
	return DescriptorsByClassPath.Num();
}

void FPlayServCodegenRegistry::GetRegisteredClassPaths(TArray<FString>& OutClassPaths)
{
	FlushPendingRegistrations();
	DescriptorsByClassPath.GetKeys(OutClassPaths);
}

void FPlayServCodegenRegistry::RegisterStruct(const TCHAR* InStructPath, const TCHAR* const* InFieldNames, int32 InFieldCount)
{
	if (!InStructPath)
	{
		return;
	}

	FPendingStructRegistration& Entry = PendingStructRegistrations.AddDefaulted_GetRef();
	Entry.StructPath = InStructPath;
	Entry.FieldNames = InFieldNames;
	Entry.FieldCount = InFieldNames ? InFieldCount : 0;

	ResolvedStructMemberNames.Empty();
}

void FPlayServCodegenRegistry::FlushPendingStructRegistrations()
{
	if (PendingStructRegistrations.Num() == 0)
	{
		return;
	}

	for (const FPendingStructRegistration& Entry : PendingStructRegistrations)
	{
		TArray<const TCHAR*>& Names = StructMemberNamesByPath.FindOrAdd(Entry.StructPath);
		Names.Reset(Entry.FieldCount);
		for (int32 i = 0; i < Entry.FieldCount; ++i)
		{
			Names.Add(Entry.FieldNames[i]);
		}
	}
	PendingStructRegistrations.Empty();
}

const TCHAR* FPlayServCodegenRegistry::FindStructMemberName(const FProperty* Property)
{
	if (!Property)
	{
		return nullptr;
	}
	const UStruct* Owner = Property->GetOwnerStruct();
	if (!Owner)
	{
		return nullptr;
	}

	FlushPendingStructRegistrations();

	TMap<FName, const TCHAR*>* Resolved = ResolvedStructMemberNames.Find(Owner);
	if (!Resolved)
	{
		Resolved = &ResolvedStructMemberNames.Add(Owner);
		if (const TArray<const TCHAR*>* Names = StructMemberNamesByPath.Find(Owner->GetPathName()))
		{
			for (const TCHAR* Name : *Names)
			{
				Resolved->Add(FName(Name), Name);
			}
		}
	}

	const TCHAR* const* Found = Resolved->Find(Property->GetFName());
	return Found ? *Found : nullptr;
}
