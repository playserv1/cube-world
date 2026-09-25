#pragma once

#include "CoreMinimal.h"

DECLARE_LOG_CATEGORY_EXTERN(LogCubeWorld, Log, All);

/**
 * True in a process that serves the game: a dedicated server, or a game build run headless as one (-cubeserver
 * with ?listen, for an engine without a Server target: the Launcher's). Such a process has no picture, no local
 * player of its own and no client session.
 */
CUBEWORLD_API bool CubeIsServerProcess();
