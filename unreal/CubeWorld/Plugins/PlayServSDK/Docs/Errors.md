# Errors and logging

## Errors

Every public callback receives an `FPlayServError` alongside `bSuccess`:

```cpp
USTRUCT(BlueprintType)
struct FPlayServError
{
    UPROPERTY() EPlayServErrorCode Code;         // the category
    UPROPERTY() FString            Message;      // what happened, for people
    UPROPERTY() FString            ProblemCode;  // the platform's own code, such as "table_write_forbidden"

    bool IsError() const;                        // Code != None
    static FPlayServError Success();
    static FPlayServError Make(Code, Message);
    static FPlayServError SubsystemUnavailable();
};
```

### Error codes: `EPlayServErrorCode`

| Code | Meaning |
|------|---------|
| `None` | Success |
| `Unknown` | Anything else: an HTTP status not listed below (400, 500, 502…), a cancelled request, or a check in the SDK such as an empty function name or a missing setting. See `Message` |
| `Unauthorized` | HTTP 401: no valid credential, an expired session, or mixed credentials |
| `Forbidden` | HTTP 403: signed in but not allowed, such as a client write to a table closed to client writes (`table_write_forbidden`), a client read of a table closed to client reads (`table_read_forbidden`), or an owner-only operation without a player session |
| `NotFound` | HTTP 404: the record, table or function slug does not exist |
| `ContractMismatch` | A typed cloud-function request or reply does not convert to or from its `USTRUCT` ([Cloud functions](CloudFunctions.md)) |
| `Timeout` | No answer within `RequestTimeoutSeconds` ([Configuration](Configuration.md)), or the platform answered 408 |
| `NetworkUnreachable` | No HTTP answer at all: connection refused, DNS failure, no route. `Message` carries the reason the HTTP stack gave |
| `Conflict` | HTTP 409: the request conflicts with the current state, such as deleting a record others still reference (`in_use`), or a schema change that needs a migration (`requires_migration`) |
| `PreconditionFailed` | HTTP 412: the record changed since you read it ([Data](Data.md)); reload and try again |
| `ValidationFailed` | HTTP 422: the request's content is invalid, such as an unknown field (`unknown_field`), an enum value the enum does not declare, a filter on a field that does not exist, or a malformed cursor. `Message` has the detail per field |

New codes are only ever added at the end; existing values never change.

### ProblemCode and Message

When the error came from the platform, `ProblemCode` carries its machine code (`not_found`, `table_write_forbidden`, `invalid_credentials`, `unknown_field`, `validation_failed`, `in_use`, `precondition_failed`, `requires_migration`, `function_unreachable`, …) and `Message` reads `HTTP <status> <code>: <detail>`, with per-field validation errors appended as ` [field: message]`. Branch on `Code` for the kind of failure and on `ProblemCode` for its exact cause. `ProblemCode` is empty when no answer arrived and for errors raised by the SDK.

Failures with no answer are classified from the reason the HTTP stack gave, never from message text, so an outage on the platform's side and a connection problem on the player's side look different in both the code and the log:

| Failure | `Code` | `Message` |
|---------|--------|-----------|
| Request exceeded the timeout | `Timeout` | `Request timed out after <elapsed>s` |
| Connection refused, DNS failure, no route, or any other failure with no answer | `NetworkUnreachable` | `Network unreachable (<reason>)` |
| Request cancelled | `Unknown` | `Request cancelled` |

### Pattern

```cpp
PlayServ::Data::Save(Entity, FPlayServSimpleCallback::CreateLambda(
    [](bool bOk, const FPlayServError& Err)
    {
        if (bOk) { return; }
        switch (Err.Code)
        {
        case EPlayServErrorCode::Unauthorized:       /* ask the player to sign in again */ break;
        case EPlayServErrorCode::PreconditionFailed: /* reload, apply again, save again */ break;
        case EPlayServErrorCode::Timeout:
        case EPlayServErrorCode::NetworkUnreachable: /* show the connection state */ break;
        default:
            UE_LOG(LogMyGame, Warning, TEXT("Save failed: %s (%s)"), *Err.Message, *Err.ProblemCode);
        }
    }));
```

## Logging

The SDK logs to a single category, `LogPlayServ`. The default verbosity is `Display`, so lifecycle events are visible without opting in.

| Verbosity | What you see |
|-----------|--------------|
| `Error` | A server key present in a process that is not a dedicated server |
| `Warning` | Every failed request (its status, or the reason no answer came), refused calls (empty ids, missing settings, unsupported credential types), a refused renewal before `OnSessionLost`, refused subscriptions, and classes the startup schema check found no table for |
| `Display` | SDK startup and shutdown (with the base URL and SDK version), login and logout with the player id, setting overrides, launcher and Steam credentials found, the startup schema check |
| `Verbose` | A line per request, `VERB /path [#N]` when sent and `VERB /path [#N] — <status> (<seconds>s)` when answered, where `#N` pairs the two; entity operations, the number of changed fields a save sends, token renewal, and the live-update connection and subscriptions |

Turn on the per-request lines from the console:

```
Log LogPlayServ Verbose
```

**What is never logged.** The SDK never logs tokens, keys, the `Authorization` header, request or response bodies, live-update messages, or URLs with query strings. Overrides of `ClientKey` and `ServerKey` log that a value was applied, never the value; the launcher exchange code and Steam ticket log only that one was found. Follow the same rule in game code ([Best practices](BestPractices.md)).

---

Part of the PlayServ SDK documentation: [README](../README.md) lists every page.
