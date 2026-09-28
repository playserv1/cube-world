#include "Data/PlayServSerializer.h"
#include "Data/PlayServCodegenRegistry.h"
#include "Data/PlayServData.h"
#include "Data/PlayServRecordIds.h"
#include "Core/PlayServLog.h"
#include "UObject/UnrealType.h"
#include "UObject/PropertyIterator.h"

#if !UE_BUILD_SHIPPING
bool FPlayServSerializer::bTestForceReflectionWalk = false;
#endif

namespace
{
	const FPlayServClassDescriptor* GetActiveDescriptor(const UClass* Class)
	{
#if !UE_BUILD_SHIPPING
		if (FPlayServSerializer::bTestForceReflectionWalk)
		{
			return nullptr;
		}
#endif
		return FPlayServCodegenRegistry::Get().FindForClass(Class);
	}
}

FString FPlayServSerializer::StructMemberWireName(const FProperty* Property)
{
	if (const TCHAR* Declared = FPlayServCodegenRegistry::Get().FindStructMemberName(Property))
	{
		return FString(Declared);
	}
	return Property->GetName();
}

TSharedPtr<FJsonObject> FPlayServSerializer::UObjectToJson(const UObject* Object)
{
	if (!Object)
	{
		return nullptr;
	}

	TSharedPtr<FJsonObject> JsonObj = MakeShared<FJsonObject>();
	const UClass* Class = Object->GetClass();

	if (const FPlayServClassDescriptor* Descriptor = GetActiveDescriptor(Class))
	{
		for (const FPlayServFieldDesc& Field : Descriptor->Fields)
		{
			FProperty* Property = Field.Property;
			if (!Property || !ensureMsgf(Field.WireName, TEXT("PlayServ: descriptor field '%s' has no wire name"), *Field.Name.ToString()))
			{
				continue;
			}

			const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
			TSharedPtr<FJsonValue> JsonValue = PropertyToJsonValue(Property, ValuePtr, Object);
			if (JsonValue.IsValid())
			{
				JsonObj->SetField(Field.WireName, JsonValue);
			}
		}
		return JsonObj;
	}

	for (TFieldIterator<FProperty> It(Class); It; ++It)
	{
		FProperty* Property = *It;

		if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_EditorOnly))
		{
			continue;
		}

		const FString FieldName = Property->GetName();
		const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);

		TSharedPtr<FJsonValue> JsonValue = PropertyToJsonValue(Property, ValuePtr, Object);
		if (JsonValue.IsValid())
		{
			JsonObj->SetField(FieldName, JsonValue);
		}
	}

	return JsonObj;
}

bool FPlayServSerializer::JsonToUObject(const TSharedPtr<FJsonObject>& JsonObject, UObject* Object)
{
	if (!JsonObject.IsValid() || !Object)
	{
		return false;
	}

	const UClass* Class = Object->GetClass();

	if (const FPlayServClassDescriptor* Descriptor = GetActiveDescriptor(Class))
	{
		for (const FPlayServFieldDesc& Field : Descriptor->Fields)
		{
			FProperty* Property = Field.Property;
			if (!Property || !ensureMsgf(Field.WireName, TEXT("PlayServ: descriptor field '%s' has no wire name"), *Field.Name.ToString()))
			{
				continue;
			}

			TSharedPtr<FJsonValue> JsonValue = JsonObject->TryGetField(Field.WireName);
			if (!JsonValue.IsValid())
			{
				continue;
			}

			void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
			JsonValueToProperty(JsonValue, Property, ValuePtr, Object);
		}
		return true;
	}

	for (TFieldIterator<FProperty> It(Class); It; ++It)
	{
		FProperty* Property = *It;

		if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_EditorOnly))
		{
			continue;
		}

		const FString FieldName = Property->GetName();

		if (!JsonObject->HasField(FieldName))
		{
			continue;
		}

		TSharedPtr<FJsonValue> JsonValue = JsonObject->TryGetField(FieldName);
		if (!JsonValue.IsValid())
		{
			continue;
		}

		void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Object);
		JsonValueToProperty(JsonValue, Property, ValuePtr, Object);
	}

	return true;
}

TSharedPtr<FJsonValue> FPlayServSerializer::PropertyToJsonValue(FProperty* Property, const void* ValuePtr, const UObject* Owner)
{
	if (FStrProperty* StrProp = CastField<FStrProperty>(Property))
	{
		return MakeShared<FJsonValueString>(StrProp->GetPropertyValue(ValuePtr));
	}

	if (FIntProperty* IntProp = CastField<FIntProperty>(Property))
	{
		return MakeShared<FJsonValueNumber>(static_cast<double>(IntProp->GetPropertyValue(ValuePtr)));
	}

	if (FInt64Property* Int64Prop = CastField<FInt64Property>(Property))
	{
		return MakeShared<FJsonValueNumber>(static_cast<double>(Int64Prop->GetPropertyValue(ValuePtr)));
	}

	if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Property))
	{
		return MakeShared<FJsonValueNumber>(static_cast<double>(FloatProp->GetPropertyValue(ValuePtr)));
	}

	if (FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Property))
	{
		return MakeShared<FJsonValueNumber>(DoubleProp->GetPropertyValue(ValuePtr));
	}

	if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Property))
	{
		return MakeShared<FJsonValueBoolean>(BoolProp->GetPropertyValue(ValuePtr));
	}

	if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Property))
	{
		const UObject* RefObj = ObjProp->GetObjectPropertyValue(ValuePtr);
		if (!RefObj)
		{
			return IsPersistentClass(ObjProp->PropertyClass)
				? TSharedPtr<FJsonValue>(MakeShared<FJsonValueNull>())
				: TSharedPtr<FJsonValue>(nullptr);
		}

		if (IsPersistentClass(RefObj->GetClass()))
		{
			return MakeShared<FJsonValueString>(FPlayServRecordIds::Get(RefObj));
		}

		return nullptr;
	}

	if (FStructProperty* StructProp = CastField<FStructProperty>(Property))
	{
		TSharedPtr<FJsonObject> StructJson = MakeShared<FJsonObject>();
		const UScriptStruct* Struct = StructProp->Struct;

		for (TFieldIterator<FProperty> It(Struct); It; ++It)
		{
			FProperty* InnerProp = *It;
			if (InnerProp->HasAnyPropertyFlags(CPF_Transient | CPF_EditorOnly))
			{
				continue;
			}
			const void* InnerPtr = InnerProp->ContainerPtrToValuePtr<void>(ValuePtr);
			TSharedPtr<FJsonValue> InnerVal = PropertyToJsonValue(InnerProp, InnerPtr, Owner);
			if (InnerVal.IsValid())
			{
				StructJson->SetField(StructMemberWireName(InnerProp), InnerVal);
			}
		}

		return MakeShared<FJsonValueObject>(StructJson);
	}

	if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property))
	{
		TArray<TSharedPtr<FJsonValue>> JsonArray;
		FScriptArrayHelper ArrayHelper(ArrayProp, ValuePtr);

		for (int32 i = 0; i < ArrayHelper.Num(); ++i)
		{
			const void* ElemPtr = ArrayHelper.GetRawPtr(i);
			TSharedPtr<FJsonValue> ElemVal = PropertyToJsonValue(ArrayProp->Inner, ElemPtr, Owner);
			if (ElemVal.IsValid())
			{
				JsonArray.Add(ElemVal);
			}
		}

		return MakeShared<FJsonValueArray>(JsonArray);
	}

	if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Property))
	{
		const int64 Value = EnumProp->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr);
		return MakeShared<FJsonValueString>(EnumProp->GetEnum()->GetNameStringByValue(Value));
	}

	return nullptr;
}

bool FPlayServSerializer::JsonValueToProperty(const TSharedPtr<FJsonValue>& JsonValue, FProperty* Property, void* ValuePtr, UObject* Owner)
{
	if (!JsonValue.IsValid())
	{
		return false;
	}

	if (FStrProperty* StrProp = CastField<FStrProperty>(Property))
	{
		FString Value;
		if (JsonValue->TryGetString(Value))
		{
			StrProp->SetPropertyValue(ValuePtr, Value);
			return true;
		}
		return false;
	}

	if (FIntProperty* IntProp = CastField<FIntProperty>(Property))
	{
		double NumVal;
		if (JsonValue->TryGetNumber(NumVal))
		{
			IntProp->SetPropertyValue(ValuePtr, static_cast<int32>(NumVal));
			return true;
		}
		return false;
	}

	if (FInt64Property* Int64Prop = CastField<FInt64Property>(Property))
	{
		double NumVal;
		if (JsonValue->TryGetNumber(NumVal))
		{
			Int64Prop->SetPropertyValue(ValuePtr, static_cast<int64>(NumVal));
			return true;
		}
		return false;
	}

	if (FFloatProperty* FloatProp = CastField<FFloatProperty>(Property))
	{
		double NumVal;
		if (JsonValue->TryGetNumber(NumVal))
		{
			FloatProp->SetPropertyValue(ValuePtr, static_cast<float>(NumVal));
			return true;
		}
		return false;
	}

	if (FDoubleProperty* DoubleProp = CastField<FDoubleProperty>(Property))
	{
		double NumVal;
		if (JsonValue->TryGetNumber(NumVal))
		{
			DoubleProp->SetPropertyValue(ValuePtr, NumVal);
			return true;
		}
		return false;
	}

	if (FBoolProperty* BoolProp = CastField<FBoolProperty>(Property))
	{
		bool BoolVal;
		if (JsonValue->TryGetBool(BoolVal))
		{
			BoolProp->SetPropertyValue(ValuePtr, BoolVal);
			return true;
		}
		return false;
	}

	if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Property))
	{
		if (JsonValue->IsNull())
		{
			ObjProp->SetObjectPropertyValue(ValuePtr, nullptr);
			return true;
		}

		FString RecordId;
		if (JsonValue->TryGetString(RecordId) && IsPersistentClass(ObjProp->PropertyClass))
		{
			if (ObjProp->PropertyClass->HasAnyClassFlags(CLASS_Abstract))
			{
				UE_LOG(LogPlayServ, Warning,
					TEXT("PlayServ Serializer: cannot instantiate abstract class '%s' for entity ref (id=%s); setting to null"),
					*ObjProp->PropertyClass->GetName(), *RecordId);
				ObjProp->SetObjectPropertyValue(ValuePtr, nullptr);
				return true;
			}
			UObject* Stub = NewObject<UObject>(Owner, ObjProp->PropertyClass);
			FPlayServRecordIds::Bind(Stub, RecordId);
			ObjProp->SetObjectPropertyValue(ValuePtr, Stub);
			return true;
		}

		return false;
	}

	if (FStructProperty* StructProp = CastField<FStructProperty>(Property))
	{
		const TSharedPtr<FJsonObject>* StructJson;
		if (JsonValue->TryGetObject(StructJson))
		{
			const UScriptStruct* Struct = StructProp->Struct;
			for (TFieldIterator<FProperty> It(Struct); It; ++It)
			{
				FProperty* InnerProp = *It;
				if (InnerProp->HasAnyPropertyFlags(CPF_Transient | CPF_EditorOnly))
				{
					continue;
				}
				TSharedPtr<FJsonValue> InnerVal = (*StructJson)->TryGetField(StructMemberWireName(InnerProp));
				if (InnerVal.IsValid())
				{
					void* InnerPtr = InnerProp->ContainerPtrToValuePtr<void>(ValuePtr);
					JsonValueToProperty(InnerVal, InnerProp, InnerPtr, Owner);
				}
			}
			return true;
		}
		return false;
	}

	if (FArrayProperty* ArrayProp = CastField<FArrayProperty>(Property))
	{
		const TArray<TSharedPtr<FJsonValue>>* JsonArray;
		if (JsonValue->TryGetArray(JsonArray))
		{
			FScriptArrayHelper ArrayHelper(ArrayProp, ValuePtr);
			ArrayHelper.Resize(JsonArray->Num());

			for (int32 i = 0; i < JsonArray->Num(); ++i)
			{
				void* ElemPtr = ArrayHelper.GetRawPtr(i);
				JsonValueToProperty((*JsonArray)[i], ArrayProp->Inner, ElemPtr, Owner);
			}
			return true;
		}
		return false;
	}

	if (FEnumProperty* EnumProp = CastField<FEnumProperty>(Property))
	{
		FString EnumStr;
		if (JsonValue->TryGetString(EnumStr))
		{
			const int64 Value = EnumProp->GetEnum()->GetValueByNameString(EnumStr);
			if (Value == INDEX_NONE)
			{
				return false;
			}
			EnumProp->GetUnderlyingProperty()->SetIntPropertyValue(ValuePtr, Value);
			return true;
		}
		return false;
	}

	return false;
}

bool FPlayServSerializer::IsPersistentClass(const UClass* Class)
{
	return UPlayServData::IsRegisteredPersistentClass(Class);
}
