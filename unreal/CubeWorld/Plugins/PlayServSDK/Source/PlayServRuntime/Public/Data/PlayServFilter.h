#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

class FPlayServFilterField;

/**
 * A query filter for LoadAll and DeleteAll: conditions on fields, all of which must hold.
 *
 * Usage:
 *   FPlayServFilter::Where("Level").GreaterThan(10)
 *   FPlayServFilter::Where("Name").EqualTo(PlayerName).And("Rarity").In({"epic","legendary"})
 *   FPlayServFilter::None()   // no filter, returns all
 *   FPlayServFilter::All()    // explicit "delete all" marker
 *
 * Comparisons follow Where() or And(); there is no OR. A dotted path reaches a field of a nested USTRUCT:
 * Where("Profile.Age"), at most 8 segments, case-sensitive. A path to no field fails with ValidationFailed, and a
 * segment through an array matches when any element does.
 */
class PLAYSERVRUNTIME_API FPlayServFilter
{
public:
	FPlayServFilter();

	/** Start a condition on a field, or on a dotted path into a nested USTRUCT. */
	static FPlayServFilterField Where(const FString& FieldName);

	/** No condition: everything matches. */
	static FPlayServFilter None();

	/** Everything, said on purpose: DeleteAll deletes every row only with this. */
	static FPlayServFilter All();

	/** Add a condition that must also hold. */
	FPlayServFilterField And(const FString& FieldName);

	/** The conditions as the platform's query takes them; empty for None() and All(). */
	TArray<TSharedPtr<FJsonValue>> ToV2FiltersArray() const;

	/** True for a filter with no conditions. */
	bool IsEmpty() const;

	/** True for All(). */
	bool IsAll() const;

private:
	friend class FPlayServFilterField;

	struct FCondition
	{
		FString Field;
		FString Op;
		TSharedPtr<FJsonValue> Value;
	};

	TArray<FCondition> Conditions;
	bool bIsNone;
	bool bIsAll;
};

/** What Where() and And() return: pick the comparison for that field. */
class PLAYSERVRUNTIME_API FPlayServFilterField
{
public:
	FPlayServFilterField(FPlayServFilter InFilter, const FString& InFieldName);

	FPlayServFilter EqualTo(const FString& Value);
	FPlayServFilter EqualTo(int32 Value);
	FPlayServFilter EqualTo(double Value);
	FPlayServFilter EqualTo(bool Value);

	FPlayServFilter NotEqualTo(const FString& Value);
	FPlayServFilter NotEqualTo(int32 Value);

	FPlayServFilter GreaterThan(int32 Value);
	FPlayServFilter GreaterThan(double Value);

	FPlayServFilter GreaterThanOrEqual(int32 Value);
	FPlayServFilter GreaterThanOrEqual(double Value);

	FPlayServFilter LessThan(int32 Value);
	FPlayServFilter LessThan(double Value);

	FPlayServFilter LessThanOrEqual(int32 Value);
	FPlayServFilter LessThanOrEqual(double Value);

	FPlayServFilter In(const TArray<FString>& Values);

	FPlayServFilter IsNull();
	FPlayServFilter IsNotNull();

private:
	void AddCondition(const FString& Op, const TSharedPtr<FJsonValue>& Value);

	FPlayServFilter Filter;
	FString FieldName;
};
