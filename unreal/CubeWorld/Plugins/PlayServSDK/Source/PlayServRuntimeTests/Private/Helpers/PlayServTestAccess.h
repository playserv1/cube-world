#pragma once

#include "Auth/PlayServAuth.h"
#include "Core/PlayServSettings.h"
#include "Data/PlayServData.h"
#include "Realtime/PlayServDataflow.h"

#if !UE_BUILD_SHIPPING
/**
 * Friend-based test access to UPlayServAuth private members.
 * Lives in the test project only — never part of the SDK. Gated to non-Shipping because the
 * SDK-side friend declaration it depends on is itself #if !UE_BUILD_SHIPPING.
 */
class FPlayServAuthTestAccess
{
public:
	/** Corrupt the stored refresh token to force the next refresh to fail. */
	static void InvalidateRefreshToken(UPlayServAuth* Auth)
	{
		Auth->RefreshToken = TEXT("invalid-token-for-testing");
	}

	/** Trigger RefreshSession() immediately, bypassing the FTSTicker timer. */
	static void ForceRefresh(UPlayServAuth* Auth)
	{
		Auth->RefreshSession();
	}

	/** Read the stored access token TTL (for test assertions). */
	static int32 GetAccessTokenTTL(const UPlayServAuth* Auth)
	{
		return Auth->AccessTokenTTL;
	}

	/** Read the stored refresh token (rotation / dead-token assertions). Never logged. */
	static const FString& GetRefreshToken(const UPlayServAuth* Auth)
	{
		return Auth->RefreshToken;
	}

	/** Read the wire-sourced refresh-token lifetime. */
	static int32 GetRefreshTokenLifetime(const UPlayServAuth* Auth)
	{
		return Auth->RefreshTokenLifetime;
	}

	/** Build the external-login request body (wire-shape pinning without a live provider token). */
	static TSharedPtr<FJsonObject> BuildV2LoginBody(const FPlayServExternalCredential& Credential)
	{
		return UPlayServAuth::BuildV2LoginBody(Credential);
	}

	/** Build the anonymous-login request body — `{}` or `{display_name}`. Same pinning, no live login. */
	static TSharedPtr<FJsonObject> BuildAnonLoginBody(const FString& DisplayName)
	{
		return UPlayServAuth::BuildAnonLoginBody(DisplayName);
	}

	/**
	 * Run the display-name sanitiser over an ARBITRARY string. Pure form for the usual reason, plus
	 * one specific to this function: the platform validates nothing, so a bug here does not fail a
	 * request — it stores the wrong name, and only a boundary test can see that.
	 */
	static FString SanitizeDisplayName(const FString& DisplayName)
	{
		return UPlayServAuth::SanitizeDisplayName(DisplayName);
	}

	/**
	 * Run the refresh-failure classification over an arbitrary error. This is the decision that
	 * used to not exist — every failure was terminal — so it is the one a regression would undo.
	 */
	static bool IsTerminalRefreshFailure(const FPlayServError& Error)
	{
		return UPlayServAuth::IsTerminalRefreshFailure(Error);
	}

	/** Is a refresh-token exchange in flight (the single-flight guard both refresh callers share)? */
	static bool IsRefreshInFlight(const UPlayServAuth* Auth)
	{
		return Auth->bRefreshInFlight;
	}

	/**
	 * Run the Epic Games Launcher command-line detection over an ARBITRARY string.
	 * The whole point of the pure form: a real launcher entry cannot be reproduced on a dev box
	 * (the one-time exchange code needs a real Epic Games Store entitlement), so the decision is
	 * proved here over synthetic command lines instead.
	 */
	static bool ParseLauncherExchangeCode(const TCHAR* CommandLine, FString& OutExchangeCode)
	{
		return UPlayServAuth::ParseLauncherExchangeCode(CommandLine, OutExchangeCode);
	}

	/**
	 * Run the Steam web-API ticket shape check over an ARBITRARY string. Same reason
	 * for the pure form as above: minting a real ticket needs a running Steam client and a real
	 * app entitlement, so the encoding decision is proved here over synthetic tickets instead.
	 */
	static bool IsWebApiTicketHex(const FString& Ticket)
	{
		return UPlayServAuth::IsWebApiTicketHex(Ticket);
	}

	/** Run the deployment-token shape check (LoginServer's second accepted credential) over an arbitrary string. */
	static bool IsJwtShaped(const FString& Credential)
	{
		return UPlayServAuth::IsJwtShaped(Credential);
	}

	/**
	 * Run the access-token `sub` decode over an ARBITRARY string. Same reason for the pure form as
	 * the parsers above: the decision is provable over synthetic tokens, with no live session and
	 * no network — and this one is the piece that can be wrong silently, since a wrong answer
	 * shows up as an empty player id rather than as a failed login.
	 */
	static bool TryParsePlayerIdFromAccessToken(const FString& AccessToken, FString& OutPlayerId)
	{
		return UPlayServAuth::TryParsePlayerIdFromAccessToken(AccessToken, OutPlayerId);
	}

	/**
	 * Drop all local session state WITHOUT signing out — the local half of a process restart.
	 *
	 * This is the only way to reach the cold-start case from inside one editor process: the tokens
	 * are gone locally while the session stays alive server-side, exactly as it is after a crash or
	 * a quit. A Logout would revoke the family and prove the opposite thing.
	 */
	static void SimulateProcessRestart(UPlayServAuth* Auth)
	{
		Auth->StopRefreshTimer();
		Auth->ClearSession();
	}
};
#endif // !UE_BUILD_SHIPPING

#if !UE_BUILD_SHIPPING
/**
 * Friend-based test access to UPlayServData internals (per-entity op queue + codegen seam).
 * Mirrors FPlayServAuthTestAccess — test project only, never part of the SDK. Gated to non-Shipping
 * because the SDK-side hooks it reaches are themselves #if !UE_BUILD_SHIPPING.
 */
struct FPlayServDataTestAccess
{
	/** Reset the dispatched-upsert counter before a coalescing burst. */
	static void ResetUpsertCount()
	{
		UPlayServData::TestDispatchedUpsertCount = 0;
	}

	/** Read the number of upserts actually dispatched since the last reset. */
	static int32 GetUpsertCount()
	{
		return UPlayServData::TestDispatchedUpsertCount;
	}

	/** Pending (not-yet-running) op count for one entity instance's queue. */
	static int32 PendingDepth(const UPlayServData* Data, const UObject* Entity)
	{
		return Data->TestPendingDepth(Entity);
	}

	/** Drain pending queue ops via shutdown semantics, then restore the transport. */
	static void DrainViaShutdown(UPlayServData* Data)
	{
		Data->TestDrainViaShutdown();
	}

	// --- Codegen seam (0.4.0) ---

	/** Force the serializer's legacy reflection walk even when a descriptor exists. */
	static void SetForceReflectionSerialization(bool bForce)
	{
		UPlayServData::TestSetForceReflectionSerialization(bForce);
	}

	/** Number of classes registered by compile-time codegen. */
	static int32 CodegenClassCount()
	{
		return UPlayServData::TestCodegenClassCount();
	}

	/** Does this class have a compile-time serializer descriptor. */
	static bool HasCodegenDescriptor(const UClass* Class)
	{
		return UPlayServData::TestClassHasCodegenDescriptor(Class);
	}

	/** Entity-level client-writability flag from the descriptor (UCLASS(PlayServClientWritable)). */
	static bool ClassClientWritable(const UClass* Class)
	{
		return UPlayServData::TestClassClientWritable(Class);
	}

	/** Partial-mode flag from the descriptor (per-property marking). */
	static bool ClassIsPartial(const UClass* Class)
	{
		return UPlayServData::TestClassIsPartial(Class);
	}

	/** Per-field client-writability flag from the descriptor. */
	static bool FieldClientWritable(const UClass* Class, const FName& FieldName)
	{
		return UPlayServData::TestFieldClientWritable(Class, FieldName);
	}

	/** Number of fields in the class's descriptor (-1 when it has none). */
	static int32 DescriptorFieldCount(const UClass* Class)
	{
		return UPlayServData::TestDescriptorFieldCount(Class);
	}

	/** Serialize through the SDK serializer (descriptor-first unless forced to reflection). */
	static TSharedPtr<FJsonObject> SerializeToJson(const UObject* Object)
	{
		return UPlayServData::TestSerializeToJson(Object);
	}

	/** Deserialize through the SDK serializer. */
	static bool DeserializeFromJson(const TSharedPtr<FJsonObject>& Json, UObject* Object)
	{
		return UPlayServData::TestDeserializeFromJson(Json, Object);
	}

	// --- Subscription seam ---

	/** Live subscription-handle count (registry size, not the WS-level record count). */
	static int32 SubscriptionHandleCount(const UPlayServData* Data)
	{
		return Data->TestSubscriptionHandleCount();
	}

	/** Close the live realtime socket as the platform does when it drains a revision, and leave the client to resume. False when no socket is open. */
	static bool SimulateRealtimeClose(UPlayServData* Data, int32 StatusCode)
	{
		if (!Data->Dataflow.IsValid())
		{
			return false;
		}
		Data->Dataflow->TestSimulatePlatformClose(StatusCode);
		return true;
	}

	/** Replace the declared member names the registry holds for one USTRUCT path. */
	static void RegisterStructMemberNames(const TCHAR* StructPath, const TCHAR* const* FieldNames, int32 FieldCount)
	{
		UPlayServData::TestRegisterStructMemberNames(StructPath, FieldNames, FieldCount);
	}

	/** Does the registry hold a declared name for every own member of this USTRUCT. */
	static bool HasStructMemberNames(const UStruct* Struct)
	{
		return UPlayServData::TestHasStructMemberNames(Struct);
	}

	/** The platform table name the schema advisory derives from a codegen class path. */
	static FString SchemaNameForClassPath(const FString& ClassPath)
	{
		return UPlayServData::SchemaNameForClassPath(ClassPath);
	}

	/** The record version tag derived from a record's updated_at (conventions §10). */
	static FString ETagForUpdatedAt(const FString& UpdatedAt)
	{
		return UPlayServData::ETagForUpdatedAt(UpdatedAt);
	}

	/** The version tag the SDK would send as If-Match for a record (empty when it holds none). */
	static FString VersionTagFor(const UPlayServData* Data, const FString& RecordId)
	{
		return Data->TestVersionTagFor(RecordId);
	}

	/** Make an in-memory object stand for an existing record — the stub the serializer builds for a reference. */
	static void BindRecordId(const UObject* Object, const FString& RecordId)
	{
		UPlayServData::TestBindRecordId(Object, RecordId);
	}
};
#endif // !UE_BUILD_SHIPPING

#if !UE_BUILD_SHIPPING
/**
 * Friend-based test access to UPlayServSettings internals. Mirrors FPlayServAuthTestAccess /
 * FPlayServDataTestAccess — test project only, never part of the SDK. Gated to non-Shipping because the
 * SDK-side friend declaration it depends on is itself #if !UE_BUILD_SHIPPING.
 */
struct FPlayServSettingsTestAccess
{
	/**
	 * Run the server-key precedence rule (command line > PLAYSERV_SERVER_KEY > ini) over ARBITRARY
	 * values. Same reason for the pure form as the launcher/Steam parsers above: the live inputs are
	 * FCommandLine and the process environment, which a test must not rewrite.
	 */
	static FString ResolveServerKey(const FString& CommandLineValue, const FString& EnvironmentValue, const FString& IniValue)
	{
		return UPlayServSettings::ResolveServerKey(CommandLineValue, EnvironmentValue, IniValue);
	}

	/** The launch's PLAYSERV_ROOM_LISTEN_PORT rule, over arbitrary text (the live input is the process environment). */
	static int32 ParseLaunchPort(const FString& Text)
	{
		return UPlayServSettings::ParseLaunchPort(Text);
	}
};
#endif // !UE_BUILD_SHIPPING
