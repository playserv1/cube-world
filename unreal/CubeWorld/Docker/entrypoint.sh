#!/bin/sh
# Starts the packaged Linux server. It listens on the port the platform allotted (process-per-room hosting) or
# 7777 (one long-lived process per machine), and logs to stdout, which is the only server log the platform keeps.
# The Launcher's engine has no Server target, so the game build runs headless as a listen server (-cubeserver,
# see CubeIsServerProcess in CubeWorld.h): no picture (-nullrhi), no sound, the local player a spectator.
set -eu
PORT="${PLAYSERV_ROOM_LISTEN_PORT:-7777}"
BIN=/server/CubeWorld/Binaries/Linux/CubeWorldServer
[ -x "$BIN" ] || BIN=/server/CubeWorld/Binaries/Linux/CubeWorld
if [ "$BIN" = /server/CubeWorld/Binaries/Linux/CubeWorldServer ]; then
    exec "$BIN" CubeWorld /Engine/Maps/Entry -port="$PORT" -log -stdout -FullStdOutLogOutput -unattended
fi
exec "$BIN" CubeWorld "/Engine/Maps/Entry?listen" -cubeserver -nullrhi -nosound -unattended -port="$PORT" -log -stdout -FullStdOutLogOutput
