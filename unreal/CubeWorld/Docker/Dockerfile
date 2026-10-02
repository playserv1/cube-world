# syntax=docker/dockerfile:1
# The Cube World dedicated server as a PlayServ machine-pool image. The build context is the Linux archive UAT
# produces and nothing else, the entrypoint is written below (RUNBOOK.md, "Part E"): the Server target from a
# source-built engine, or, from the Launcher's engine (no Server target), the Game target run headless:
#   RunUAT.bat BuildCookRun -project=<repo>\unreal\CubeWorld\CubeWorld.uproject -target=CubeWorldServer -server
#     -serverplatform=Linux -noclient -serverconfig=Development -build -cook -stage -pak -archive -archivedirectory=<out>
#   playserv image push --slug cubeworld-ue --src <out>\LinuxServer --dockerfile <repo>\unreal\CubeWorld\Docker\Dockerfile --tag <tag>
# The platform runs it with host networking and no arguments, and hands the machine over in the environment
# (PLAYSERV_DEPLOYMENT_TOKEN, PLAYSERV_API_URL, PLAYSERV_EXECUTOR_SLUG, PLAYSERV_MACHINE_ID, PLAYSERV_PUBLIC_IP,
# PLAYSERV_ROOM_LISTEN_PORT). Arguments given to the container go on the server's command line, for a run on a
# developer's machine: docker run --rm -p 7777:7777/udp -p 7787:7787 <image> -cubeoffline -region=3.

# The archive, less what a server never needs at run time: the debugger's symbols (*.debug, over half the archive)
# and the Vulkan layers (the server draws nothing). Pruned in a stage of its own, so the image every machine of the
# pool pulls is small whatever the build context held. *.sym stays: with it the engine names the functions of a
# crash's callstack in the log.
FROM ubuntu:22.04 AS archive
COPY . /server
RUN find /server -name '*.debug' -delete && rm -rf /server/Engine/Binaries/ThirdParty/Vulkan

FROM ubuntu:22.04
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates libc++1 libc++abi1 && rm -rf /var/lib/apt/lists/* \
    && useradd -m -u 1000 server
COPY --from=archive --chown=server:server /server /server

# Starts the packaged Linux server. It listens on the port the platform allotted (process-per-room hosting) or 7777
# (one long-lived process per machine), Iris on UDP and the JSON door on TCP, and logs to stdout, which is the only
# server log the platform keeps. The Launcher's engine has no Server target, so its game build runs headless as a
# listen server (-cubeserver, see CubeIsServerProcess in CubeWorld.h): no picture, no sound, the local player a
# spectator.
COPY --chmod=755 <<'EOF' /server/entrypoint.sh
#!/bin/sh
set -eu
PORT="${PLAYSERV_ROOM_LISTEN_PORT:-7777}"
BIN=/server/CubeWorld/Binaries/Linux/CubeWorldServer
[ -x "$BIN" ] || BIN=/server/CubeWorld/Binaries/Linux/CubeWorld
if [ "$BIN" = /server/CubeWorld/Binaries/Linux/CubeWorldServer ]; then
    exec "$BIN" CubeWorld /Engine/Maps/Entry -port="$PORT" -log -stdout -FullStdOutLogOutput -unattended "$@"
fi
exec "$BIN" CubeWorld "/Engine/Maps/Entry?listen" -cubeserver -nullrhi -nosound -unattended -port="$PORT" -log -stdout -FullStdOutLogOutput "$@"
EOF
# A checkout on Windows may still hand this file over with CRLF line endings (.gitattributes keeps it LF), and a script
# that starts "#!/bin/sh\r" cannot be run at all.
RUN sed -i 's/\r$//' /server/entrypoint.sh
USER server
WORKDIR /server
# With no allotted port: Iris on UDP 7777, the door on TCP 7787 (ten above). On the pool both are on the allotted one.
EXPOSE 7777/udp 7787/tcp
ENTRYPOINT ["/server/entrypoint.sh"]
