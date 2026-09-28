#pragma once

#include "CoreMinimal.h"

// Warning: the signature is not verified; never base a trust decision on a claim read here.
namespace PlayServJwt
{
	bool IsShaped(const FString& Token);

	bool TryReadStringClaim(const FString& Token, const TCHAR* Claim, FString& OutValue);
}
