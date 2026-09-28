#pragma once

#include "CoreMinimal.h"
#include "PlayServTypes.generated.h"

/** The kind of session: a player's, or a server's. */
UENUM(BlueprintType)
enum class EPlayServSessionType : uint8
{
	/** No active session. */
	None,
	/** A player's session: it has a player id and renews itself. */
	Client,
	/** A server session: no player id, no renewal. */
	Server
};

/** Error categories for PlayServ callbacks. FPlayServError::ProblemCode carries the platform's precise code. */
// Warning: append new codes at the end; the values are serialized and used by Blueprints.
UENUM(BlueprintType)
enum class EPlayServErrorCode : uint8
{
	/** No error. */
	None,
	/** Any other failure; Message says what happened. */
	Unknown,
	/** No session, or the session was refused (HTTP 401). */
	Unauthorized,
	/** The thing asked for does not exist, such as a record or a cloud function. */
	NotFound,
	/**
	 * A typed cloud-function request or reply does not match its USTRUCT: a field of the wrong type, or a request that
	 * does not serialize. See PlayServ::Code::Call.
	 */
	ContractMismatch,
	/** No answer arrived within UPlayServSettings::RequestTimeoutSeconds. */
	Timeout,
	/**
	 * The request never reached the platform: the connection failed, or the SDK refused it before sending. A credential
	 * the platform never saw is still valid, so with this code and with Timeout keep it and try again later; see
	 * LoginWithRefreshToken.
	 */
	NetworkUnreachable,
	/** Signed in but not allowed (HTTP 403), such as a client write to a table closed to client writes (`table_write_forbidden`). */
	Forbidden,
	/** The request conflicts with the platform's current state (HTTP 409), such as `requires_migration`. */
	Conflict,
	/** Someone changed the record since you read it (HTTP 412): Reload, apply your change again, and save. */
	PreconditionFailed,
	/** The request's content is invalid (HTTP 422), such as `unknown_field`; ProblemCode names it and Message has the detail. */
	ValidationFailed
};

/** The error every PlayServ callback carries. On success Code is None and Message is empty. */
USTRUCT(BlueprintType)
struct PLAYSERVRUNTIME_API FPlayServError
{
	GENERATED_BODY()

	/** The error category. */
	UPROPERTY(BlueprintReadOnly, Category="PlayServ")
	EPlayServErrorCode Code = EPlayServErrorCode::None;

	/** What happened, for people. */
	UPROPERTY(BlueprintReadOnly, Category="PlayServ")
	FString Message;

	/**
	 * The platform's own code for the error, such as "not_found", "table_write_forbidden" or "requires_migration"; empty
	 * when the error did not come from the platform. Branch on it for precise handling; Code is the category.
	 */
	UPROPERTY(BlueprintReadOnly, Category="PlayServ")
	FString ProblemCode;

	/** True for an error. */
	bool IsError() const { return Code != EPlayServErrorCode::None; }

	/** A success. */
	static FPlayServError Success() { return FPlayServError(); }

	/** An error with this code and message. */
	static FPlayServError Make(EPlayServErrorCode InCode, const FString& InMessage)
	{
		FPlayServError Err;
		Err.Code = InCode;
		Err.Message = InMessage;
		return Err;
	}

	/** The error every PlayServ call reports when the SDK is not running, such as before startup or in an editor utility. */
	static FPlayServError SubsystemUnavailable()
	{
		return Make(EPlayServErrorCode::Unknown, TEXT("PlayServ subsystem not available"));
	}
};

/** A callback with the outcome and no result. */
DECLARE_DELEGATE_TwoParams(FPlayServSimpleCallback, bool /*bSuccess*/, const FPlayServError& /*Error*/);
