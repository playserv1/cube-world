#pragma once

#include "Core/PlayServTypes.h"
#include "Interfaces/IHttpBase.h"

namespace PlayServErrorMapping
{
	inline FPlayServError FromV2Response(int32 Status, const FString& ProblemCode, const FString& Detail)
	{
		EPlayServErrorCode Code;
		switch (Status)
		{
		case 401: Code = EPlayServErrorCode::Unauthorized; break;
		case 403: Code = EPlayServErrorCode::Forbidden; break;
		case 404: Code = EPlayServErrorCode::NotFound; break;
		case 408: Code = EPlayServErrorCode::Timeout; break;
		case 409: Code = EPlayServErrorCode::Conflict; break;
		case 412: Code = EPlayServErrorCode::PreconditionFailed; break;
		case 422: Code = EPlayServErrorCode::ValidationFailed; break;
		default:  Code = EPlayServErrorCode::Unknown; break;
		}

		FString Message = FString::Printf(TEXT("HTTP %d"), Status);
		if (!ProblemCode.IsEmpty())
		{
			Message += FString::Printf(TEXT(" %s"), *ProblemCode);
		}
		if (!Detail.IsEmpty())
		{
			Message += FString::Printf(TEXT(": %s"), *Detail);
		}

		FPlayServError Err = FPlayServError::Make(Code, Message);
		Err.ProblemCode = ProblemCode;
		Err.HttpStatus = Status;
		return Err;
	}

	inline FPlayServError FromTransportFailure(EHttpFailureReason Reason, double ElapsedSeconds)
	{
		switch (Reason)
		{
		case EHttpFailureReason::TimedOut:
			return FPlayServError::Make(EPlayServErrorCode::Timeout,
				FString::Printf(TEXT("Request timed out after %.1fs"), ElapsedSeconds));
		case EHttpFailureReason::Cancelled:
			return FPlayServError::Make(EPlayServErrorCode::Unknown, TEXT("Request cancelled"));
		default:
			return FPlayServError::Make(EPlayServErrorCode::NetworkUnreachable,
				FString::Printf(TEXT("Network unreachable (%s)"), LexToString(Reason)));
		}
	}
}
