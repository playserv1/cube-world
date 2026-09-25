#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * Schema pusher — reads the UHT-emitted PlayServManifest.json (v2) plus the
 * Config/PlayServSchemaOverlay.json overlay, converts them to the platform's
 * CodeSchemaPushDto, exchanges the sk_ server key for an operator session
 * (POST /api/v1/auth/cli) and POSTs /api/v1/schema:push-from-code with the current
 * expected_revision. Prints a diff-style result (created / updated / unchanged per element).
 *
 * SYNCHRONOUS by design: runs from the commandlet (headless, ticks the HTTP manager itself)
 * and from the editor Tools-menu action (operator-initiated, blocking for the ~2s round
 * trips is acceptable and keeps one code path).
 */
class FPlayServSchemaPusher
{
public:
	struct FResult
	{
		bool bSuccess = false;
		FString Summary;
	};

	/**
	 * Run the full push. BaseURL/keys resolve from UPlayServSettings + [PlayServ.DevSecrets]
	 * unless overridden (-PlayServBaseURL= / -PlayServServerKey= / -ManifestPath= are honored
	 * by the commandlet before calling this).
	 */
	static FResult Run(const FString& ManifestPathOverride, const FString& ServerKeyOverride, const FString& BaseURLOverride, bool bDryRun = false, const FString& OverlayPathOverride = FString());

private:
	/**
	 * The offline test seam (PlayServ.Editor.SchemaPush.*): the friend forwards BuildPushDto —
	 * pure JSON in, JSON out, no network — so the body shape is pinned without a platform round
	 * trip. Same pattern as FPlayServAuthTestAccess; no Shipping gate because an Editor module
	 * never builds Shipping.
	 */
	friend class FPlayServSchemaPusherTestAccess;

	static FString ResolveManifestPath(const FString& Override);
	static TSharedPtr<FJsonObject> LoadJsonFile(const FString& Path);

	/** Blocking HTTP round trip that works in a commandlet (ticks the HTTP manager). */
	static bool SyncRequest(const FString& Verb, const FString& Url, const TArray<TPair<FString, FString>>& Headers,
		const FString& Body, int32& OutStatus, TSharedPtr<FJsonObject>& OutJson, FString& OutRaw);

	/** Manifest + overlay → the REST CodeSchemaPushDto (snake_case) minus expected_revision. */
	static TSharedPtr<FJsonObject> BuildPushDto(const TSharedPtr<FJsonObject>& Manifest, const TSharedPtr<FJsonObject>& Overlay, FString& OutError);

	/** Map one manifest field (kindName + decl) to a platform field object, or null to skip. */
	static TSharedPtr<FJsonObject> MapField(const TSharedPtr<FJsonObject>& ManifestField, const TSet<FString>& EntityNames, FString& OutWarning);
};
