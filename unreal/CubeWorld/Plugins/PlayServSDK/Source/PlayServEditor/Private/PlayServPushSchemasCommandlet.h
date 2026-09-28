#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "PlayServPushSchemasCommandlet.generated.h"

/**
 * Headless schema push:
 *   UnrealEditor-Cmd.exe <project> -run=PlayServPushSchemas
 *     [-ManifestPath=<path>] [-PlayServServerKey=sk_...] [-PlayServBaseURL=https://...]
 *
 * Reads PlayServManifest.json (newest under the plugin's Intermediate tree) plus
 * Config/PlayServSchemaOverlay.json, exchanges the sk_ for an operator session and POSTs
 * /api/v1/schema:push-from-code with the current expected_revision. Prints a diff-style
 * result; exit code 0 only on a successful push.
 */
UCLASS()
class UPlayServPushSchemasCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	virtual int32 Main(const FString& Params) override;
};
