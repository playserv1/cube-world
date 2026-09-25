#include "Data/PlayServFilter.h"

FPlayServFilter::FPlayServFilter()
	: bIsNone(false)
	, bIsAll(false)
{
}

FPlayServFilterField FPlayServFilter::Where(const FString& FieldName)
{
	return FPlayServFilterField(FPlayServFilter(), FieldName);
}

FPlayServFilter FPlayServFilter::None()
{
	FPlayServFilter Filter;
	Filter.bIsNone = true;
	return Filter;
}

FPlayServFilter FPlayServFilter::All()
{
	FPlayServFilter Filter;
	Filter.bIsAll = true;
	return Filter;
}

FPlayServFilterField FPlayServFilter::And(const FString& FieldName)
{
	return FPlayServFilterField(*this, FieldName);
}

TArray<TSharedPtr<FJsonValue>> FPlayServFilter::ToV2FiltersArray() const
{
	TArray<TSharedPtr<FJsonValue>> Filters;
	if (bIsNone || bIsAll)
	{
		return Filters;
	}
	for (const FCondition& Condition : Conditions)
	{
		TSharedPtr<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("field"), Condition.Field);
		Entry->SetStringField(TEXT("op"), Condition.Op);
		if (Condition.Value.IsValid())
		{
			Entry->SetField(TEXT("value"), Condition.Value);
		}
		Filters.Add(MakeShared<FJsonValueObject>(Entry));
	}
	return Filters;
}

bool FPlayServFilter::IsEmpty() const
{
	return bIsNone || (!bIsAll && Conditions.Num() == 0);
}

bool FPlayServFilter::IsAll() const
{
	return bIsAll;
}

FPlayServFilterField::FPlayServFilterField(FPlayServFilter InFilter, const FString& InFieldName)
	: Filter(MoveTemp(InFilter))
	, FieldName(InFieldName)
{
}

FPlayServFilter FPlayServFilterField::EqualTo(const FString& Value)
{
	AddCondition(TEXT("eq"), MakeShared<FJsonValueString>(Value));
	return Filter;
}

FPlayServFilter FPlayServFilterField::EqualTo(int32 Value)
{
	AddCondition(TEXT("eq"), MakeShared<FJsonValueNumber>(static_cast<double>(Value)));
	return Filter;
}

FPlayServFilter FPlayServFilterField::EqualTo(double Value)
{
	AddCondition(TEXT("eq"), MakeShared<FJsonValueNumber>(Value));
	return Filter;
}

FPlayServFilter FPlayServFilterField::EqualTo(bool Value)
{
	AddCondition(TEXT("eq"), MakeShared<FJsonValueBoolean>(Value));
	return Filter;
}

FPlayServFilter FPlayServFilterField::NotEqualTo(const FString& Value)
{
	AddCondition(TEXT("neq"), MakeShared<FJsonValueString>(Value));
	return Filter;
}

FPlayServFilter FPlayServFilterField::NotEqualTo(int32 Value)
{
	AddCondition(TEXT("neq"), MakeShared<FJsonValueNumber>(static_cast<double>(Value)));
	return Filter;
}

FPlayServFilter FPlayServFilterField::GreaterThan(int32 Value)
{
	AddCondition(TEXT("gt"), MakeShared<FJsonValueNumber>(static_cast<double>(Value)));
	return Filter;
}

FPlayServFilter FPlayServFilterField::GreaterThan(double Value)
{
	AddCondition(TEXT("gt"), MakeShared<FJsonValueNumber>(Value));
	return Filter;
}

FPlayServFilter FPlayServFilterField::GreaterThanOrEqual(int32 Value)
{
	AddCondition(TEXT("gte"), MakeShared<FJsonValueNumber>(static_cast<double>(Value)));
	return Filter;
}

FPlayServFilter FPlayServFilterField::GreaterThanOrEqual(double Value)
{
	AddCondition(TEXT("gte"), MakeShared<FJsonValueNumber>(Value));
	return Filter;
}

FPlayServFilter FPlayServFilterField::LessThan(int32 Value)
{
	AddCondition(TEXT("lt"), MakeShared<FJsonValueNumber>(static_cast<double>(Value)));
	return Filter;
}

FPlayServFilter FPlayServFilterField::LessThan(double Value)
{
	AddCondition(TEXT("lt"), MakeShared<FJsonValueNumber>(Value));
	return Filter;
}

FPlayServFilter FPlayServFilterField::LessThanOrEqual(int32 Value)
{
	AddCondition(TEXT("lte"), MakeShared<FJsonValueNumber>(static_cast<double>(Value)));
	return Filter;
}

FPlayServFilter FPlayServFilterField::LessThanOrEqual(double Value)
{
	AddCondition(TEXT("lte"), MakeShared<FJsonValueNumber>(Value));
	return Filter;
}

FPlayServFilter FPlayServFilterField::In(const TArray<FString>& Values)
{
	ensureMsgf(!Values.IsEmpty(), TEXT("PlayServ: FPlayServFilter::In() called with empty array — will match no documents."));

	TArray<TSharedPtr<FJsonValue>> JsonValues;
	for (const FString& Val : Values)
	{
		JsonValues.Add(MakeShared<FJsonValueString>(Val));
	}
	AddCondition(TEXT("in"), MakeShared<FJsonValueArray>(JsonValues));
	return Filter;
}

FPlayServFilter FPlayServFilterField::IsNull()
{
	AddCondition(TEXT("is_null"), nullptr);
	return Filter;
}

FPlayServFilter FPlayServFilterField::IsNotNull()
{
	AddCondition(TEXT("is_not_null"), nullptr);
	return Filter;
}

void FPlayServFilterField::AddCondition(const FString& Op, const TSharedPtr<FJsonValue>& Value)
{
	FPlayServFilter::FCondition Condition;
	Condition.Field = FieldName;
	Condition.Op = Op;
	Condition.Value = Value;
	Filter.Conditions.Add(MoveTemp(Condition));
}
