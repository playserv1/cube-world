#pragma once

#include "CoreMinimal.h"
#include "PlayServRpcTestTypes.generated.h"

// USTRUCT request/response types for the PlayServ.Code.TypedRPC.* autotests.
// Field names are PascalCase and go on the wire verbatim — for the live-function types they
// name the same fields as the deployed reference functions' contracts. Casing is tolerant on
// the read side — FJsonObject::TryGetField is case-insensitive, so the functions'
// [JsonPropertyName] PascalCase replies are belt-and-braces (see CamelCaseReplyBinds):
//   create-player: req { DisplayName } -> reply { Id, Created }
//   buy-upgrade:   req { UpgradeType } -> reply { Currency, UpgradeLevels, PurchasedType, NewLevel, Cost }
// The caller's identity comes from the gateway (no PlayerId in any request).

/** Mirrors the create-player request. */
USTRUCT()
struct FCreatePlayerFnRequest
{
	GENERATED_BODY()

	UPROPERTY() FString DisplayName;
};

/** Mirrors the create-player reply. */
USTRUCT()
struct FCreatePlayerFnResponse
{
	GENERATED_BODY()

	UPROPERTY() FString Id;
	UPROPERTY() bool Created = false;
};

/** Mirrors the buy-upgrade request. */
USTRUCT()
struct FBuyUpgradeRequest
{
	GENERATED_BODY()

	UPROPERTY() int32 UpgradeType = 0;
};

/** Mirrors the buy-upgrade success reply. */
USTRUCT()
struct FBuyUpgradeResponse
{
	GENERATED_BODY()

	UPROPERTY() int32 Currency = 0;
	UPROPERTY() TArray<int32> UpgradeLevels;
	UPROPERTY() int32 PurchasedType = 0;
	UPROPERTY() int32 NewLevel = 0;
	UPROPERTY() int32 Cost = 0;
};

// ---------------------------------------------------------------------------
// Isolated converter-test types (no C# counterpart)
// ---------------------------------------------------------------------------

/** enum-by-name round-trip probe. */
UENUM()
enum class ERpcTestColor : uint8
{
	Red,
	Green,
	Blue
};

/** Nested USTRUCT element. */
USTRUCT()
struct FRpcTestInner
{
	GENERATED_BODY()

	UPROPERTY() FString Label;
	UPROPERTY() int32 Weight = 0;
};

/** Composite for the nested-USTRUCT + TArray + enum-by-name round-trip test. */
USTRUCT()
struct FRpcTestComposite
{
	GENERATED_BODY()

	UPROPERTY() FString Name;
	UPROPERTY() int32 Count = 0;
	UPROPERTY() ERpcTestColor Color = ERpcTestColor::Red;
	UPROPERTY() FRpcTestInner Inner;
	UPROPERTY() TArray<int32> Numbers;
	UPROPERTY() TArray<FRpcTestInner> Items;
};

/**
 * Strict-reject probe. Non-default C++ initializers (Quantity=7, bEnabled=true) let the
 * absent-field test assert defaults are retained when a field is missing from the JSON.
 */
USTRUCT()
struct FRpcStrictProbe
{
	GENERATED_BODY()

	UPROPERTY() int32 Quantity = 7;
	UPROPERTY() bool bEnabled = true;
	UPROPERTY() FString Tag;
};
