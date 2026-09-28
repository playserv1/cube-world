#pragma once

#include "CoreMinimal.h"
#include "Core/PlayServTypes.h"

class FPlayServBatchCounter
{
public:
	FPlayServBatchCounter(int32 InTotal, FPlayServSimpleCallback InCallback)
		: Remaining(InTotal)
		, bAnyFailed(false)
		, Callback(MoveTemp(InCallback))
	{
	}

	void Complete(bool bSuccess, const FPlayServError& Error)
	{
		if (!bSuccess && !bAnyFailed)
		{
			bAnyFailed = true;
			FirstError = Error;
		}

		if (--Remaining <= 0)
		{
			Callback.ExecuteIfBound(!bAnyFailed, FirstError);
		}
	}

private:
	int32 Remaining;
	bool bAnyFailed;
	FPlayServError FirstError;
	FPlayServSimpleCallback Callback;
};
