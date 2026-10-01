#pragma once

#include "CoreMinimal.h"

DECLARE_LOG_CATEGORY_EXTERN(LogCubeWorld, Log, All);

/**
 * True in a process that serves the game: a dedicated server, or a game build run headless as one (-cubeserver
 * with ?listen, for an engine without a Server target: the Launcher's). Such a process has no picture, no local
 * player of its own and no client session.
 */
CUBEWORLD_API bool CubeIsServerProcess();

/**
 * True when started with -cubeoffline: a run on one machine that never touches the platform, for checking servers and
 * crossings without signing in to the shared, live `dev`. Servers take their region from -region=<n>; servers and
 * clients take the servers' addresses from -peers=<region>@<host>:<port>,... A client signs in as nobody and travels
 * straight to an address; a server checks no ticket, shares nothing, and hands everyone the starting inventory.
 */
CUBEWORLD_API bool CubeIsOffline();

struct FCubeOfflinePeer
{
	int32 Region = -1;
	/** host:port of the server holding the region. */
	FString Address;
};

/** The servers of an offline run, by region, from -peers=. */
CUBEWORLD_API const TArray<FCubeOfflinePeer>& CubeOfflinePeers();

/** An offline server's room, the same on every process of the run: <colour>-offline. */
CUBEWORLD_API FString CubeOfflineRoomName(int32 Region);
