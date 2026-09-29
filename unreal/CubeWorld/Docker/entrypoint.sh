#!/bin/sh
# Starts the packaged Linux server. It listens on the port the platform allotted (process-per-room hosting) or
# 7777 (one long-lived process per machine), and logs to stdout, which is the only server log the platform keeps.
set -eu
PORT="${PLAYSERV_ROOM_LISTEN_PORT:-7777}"
exec /server/CubeWorld/Binaries/Linux/CubeWorldServer CubeWorld /Engine/Maps/Entry -port="$PORT" -log -stdout -FullStdOutLogOutput -unattended
