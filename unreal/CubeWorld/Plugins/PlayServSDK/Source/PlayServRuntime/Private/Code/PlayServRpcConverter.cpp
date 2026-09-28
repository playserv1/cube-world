#include "Code/PlayServRpcConverter.h"

#include "Data/PlayServSerializer.h"
#include "Dom/JsonValue.h"
#include "UObject/UnrealType.h"
#include "UObject/StrProperty.h"
#include "UObject/EnumProperty.h"
#include "Templates/SharedPointer.h"

namespace
{
	TSharedPtr<FJsonValue> PropertyToJsonValue(const FProperty* Property, const void* ValuePtr);
	bool JsonValueToProperty(const TSharedPtr<FJsonValue>& JsonValue, const FProperty* Property, void* ValuePtr);

	TSharedPtr<FJsonObject> StructToJsonObject(const UStruct* StructType, const void* StructData)
	{
		TSharedPtr<FJsonObject> Json = MakeShared<FJsonObject>();
		for (TFieldIterator<FProperty> It(StructType); It; ++It)
		{
			const FProperty* Property = *It;
			if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_EditorOnly))
			{
				continue;
			}
			const TSharedPtr<FJsonValue> Value = PropertyToJsonValue(Property, Property->ContainerPtrToValuePtr<void>(StructData));
			if (Value.IsValid())
			{
				Json->SetField(FPlayServSerializer::StructMemberWireName(Property), Value);
			}
		}
		return Json;
	}

	bool JsonToStructStrict(const TSharedPtr<FJsonObject>& Json, const UStruct* StructType, void* OutStructData)
	{
		if (!Json.IsValid())
		{
			return false;
		}
		bool bAllConverted = true;
		for (TFieldIterator<FProperty> It(StructType); It; ++It)
		{
			const FProperty* Property = *It;
			if (Property->HasAnyPropertyFlags(CPF_Transient | CPF_EditorOnly))
			{
				continue;
			}
			const TSharedPtr<FJsonValue> Found = Json->TryGetField(FPlayServSerializer::StructMemberWireName(Property));
			if (Found.IsValid())
			{
				bAllConverted &= JsonValueToProperty(Found, Property, Property->ContainerPtrToValuePtr<void>(OutStructData));
			}
		}
		return bAllConverted;
	}

	TSharedPtr<FJsonValue> PropertyToJsonValue(const FProperty* Property, const void* ValuePtr)
	{
		if (const FStrProperty* StrProperty = CastField<FStrProperty>(Property))
		{
			return MakeShared<FJsonValueString>(StrProperty->GetPropertyValue(ValuePtr));
		}
		if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		{
			return MakeShared<FJsonValueBoolean>(BoolProperty->GetPropertyValue(ValuePtr));
		}
		if (const FIntProperty* IntProperty = CastField<FIntProperty>(Property))
		{
			return MakeShared<FJsonValueNumber>(static_cast<double>(IntProperty->GetPropertyValue(ValuePtr)));
		}
		if (const FInt64Property* Int64Property = CastField<FInt64Property>(Property))
		{
			return MakeShared<FJsonValueNumber>(static_cast<double>(Int64Property->GetPropertyValue(ValuePtr)));
		}
		if (const FFloatProperty* FloatProperty = CastField<FFloatProperty>(Property))
		{
			return MakeShared<FJsonValueNumber>(static_cast<double>(FloatProperty->GetPropertyValue(ValuePtr)));
		}
		if (const FDoubleProperty* DoubleProperty = CastField<FDoubleProperty>(Property))
		{
			return MakeShared<FJsonValueNumber>(DoubleProperty->GetPropertyValue(ValuePtr));
		}
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			const int64 EnumValue = EnumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(ValuePtr);
			return MakeShared<FJsonValueString>(EnumProperty->GetEnum()->GetNameStringByValue(EnumValue));
		}
		if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			if (ByteProperty->Enum != nullptr)
			{
				return MakeShared<FJsonValueString>(ByteProperty->Enum->GetNameStringByValue(ByteProperty->GetPropertyValue(ValuePtr)));
			}
			return MakeShared<FJsonValueNumber>(static_cast<double>(ByteProperty->GetPropertyValue(ValuePtr)));
		}
		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			return MakeShared<FJsonValueObject>(StructToJsonObject(StructProperty->Struct, ValuePtr));
		}
		if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			FScriptArrayHelper Helper(ArrayProperty, ValuePtr);
			TArray<TSharedPtr<FJsonValue>> Values;
			Values.Reserve(Helper.Num());
			for (int32 Index = 0; Index < Helper.Num(); ++Index)
			{
				TSharedPtr<FJsonValue> Element = PropertyToJsonValue(ArrayProperty->Inner, Helper.GetRawPtr(Index));
				if (Element.IsValid())
				{
					Values.Add(MoveTemp(Element));
				}
			}
			return MakeShared<FJsonValueArray>(Values);
		}
		return nullptr;
	}

	bool JsonValueToProperty(const TSharedPtr<FJsonValue>& JsonValue, const FProperty* Property, void* ValuePtr)
	{
		if (const FStrProperty* StrProperty = CastField<FStrProperty>(Property))
		{
			if (JsonValue->Type != EJson::String)
			{
				return false;
			}
			StrProperty->SetPropertyValue(ValuePtr, JsonValue->AsString());
			return true;
		}
		if (const FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		{
			if (JsonValue->Type != EJson::Boolean)
			{
				return false;
			}
			BoolProperty->SetPropertyValue(ValuePtr, JsonValue->AsBool());
			return true;
		}
		if (const FIntProperty* IntProperty = CastField<FIntProperty>(Property))
		{
			int32 Value = 0;
			if (JsonValue->Type == EJson::Number && JsonValue->TryGetNumber(Value))
			{
				IntProperty->SetPropertyValue(ValuePtr, Value);
				return true;
			}
			return false;
		}
		if (const FInt64Property* Int64Property = CastField<FInt64Property>(Property))
		{
			int64 Value = 0;
			if (JsonValue->Type == EJson::Number && JsonValue->TryGetNumber(Value))
			{
				Int64Property->SetPropertyValue(ValuePtr, Value);
				return true;
			}
			return false;
		}
		if (const FFloatProperty* FloatProperty = CastField<FFloatProperty>(Property))
		{
			if (JsonValue->Type != EJson::Number)
			{
				return false;
			}
			FloatProperty->SetPropertyValue(ValuePtr, static_cast<float>(JsonValue->AsNumber()));
			return true;
		}
		if (const FDoubleProperty* DoubleProperty = CastField<FDoubleProperty>(Property))
		{
			if (JsonValue->Type != EJson::Number)
			{
				return false;
			}
			DoubleProperty->SetPropertyValue(ValuePtr, JsonValue->AsNumber());
			return true;
		}
		if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			if (JsonValue->Type == EJson::String)
			{
				const int64 EnumValue = EnumProperty->GetEnum()->GetValueByNameString(JsonValue->AsString());
				if (EnumValue != INDEX_NONE)
				{
					EnumProperty->GetUnderlyingProperty()->SetIntPropertyValue(ValuePtr, EnumValue);
					return true;
				}
			}
			return false;
		}
		if (const FByteProperty* ByteProperty = CastField<FByteProperty>(Property))
		{
			if (ByteProperty->Enum != nullptr)
			{
				if (JsonValue->Type == EJson::String)
				{
					const int64 EnumValue = ByteProperty->Enum->GetValueByNameString(JsonValue->AsString());
					if (EnumValue != INDEX_NONE)
					{
						ByteProperty->SetPropertyValue(ValuePtr, static_cast<uint8>(EnumValue));
						return true;
					}
				}
				return false;
			}
			int32 Value = 0;
			if (JsonValue->Type == EJson::Number && JsonValue->TryGetNumber(Value))
			{
				ByteProperty->SetPropertyValue(ValuePtr, static_cast<uint8>(Value));
				return true;
			}
			return false;
		}
		if (const FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (JsonValue->TryGetObject(Object))
			{
				return JsonToStructStrict(*Object, StructProperty->Struct, ValuePtr);
			}
			return false;
		}
		if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			const TArray<TSharedPtr<FJsonValue>>* Elements = nullptr;
			if (JsonValue->TryGetArray(Elements))
			{
				FScriptArrayHelper Helper(ArrayProperty, ValuePtr);
				Helper.Resize(Elements->Num());
				bool bAllConverted = true;
				for (int32 Index = 0; Index < Elements->Num(); ++Index)
				{
					bAllConverted &= JsonValueToProperty((*Elements)[Index], ArrayProperty->Inner, Helper.GetRawPtr(Index));
				}
				return bAllConverted;
			}
			return false;
		}
		return false;
	}
}

namespace PlayServ::Code
{
	bool StructToRpcParams(const UStruct* Struct, const void* Value, FPlayServRPCParams& OutParams)
	{
		if (Struct == nullptr || Value == nullptr)
		{
			return false;
		}
		const TSharedPtr<FJsonObject> Json = StructToJsonObject(Struct, Value);
		if (!Json.IsValid())
		{
			return false;
		}
		for (const auto& Field : Json->Values)
		{
			OutParams.Set(*Field.Key, Field.Value);
		}
		return true;
	}

	bool JsonObjectToStruct(const TSharedPtr<FJsonObject>& Json, const UStruct* Struct, void* OutValue)
	{
		if (Struct == nullptr || OutValue == nullptr)
		{
			return false;
		}
		return JsonToStructStrict(Json, Struct, OutValue);
	}
}
