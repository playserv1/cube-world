# The Cube World dedicated server as a PlayServ machine-pool image. The build context is the Linux archive UAT
# produces (RUNBOOK.md, "The Unreal dedicated server on the platform"): the Server target from a source-built
# engine, or, from the Launcher's engine (no Server target), the Game target run headless (entrypoint.sh):
#   RunUAT.bat BuildCookRun -project=<repo>\unreal\CubeWorld\CubeWorld.uproject -platform=Linux -clientconfig=Development
#     -build -cook -stage -pak -archive -archivedirectory=<out>
#   playserv image push --slug cubeworld-ue --src <out>\Linux --dockerfile <repo>\unreal\CubeWorld\Docker\Dockerfile --tag <tag>
# The platform runs it with host networking and no arguments, and hands the machine over in the environment
# (PLAYSERV_DEPLOYMENT_TOKEN, PLAYSERV_API_URL, PLAYSERV_EXECUTOR_SLUG, PLAYSERV_MACHINE_ID, PLAYSERV_PUBLIC_IP).
FROM ubuntu:22.04
RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates libc++1 libc++abi1 && rm -rf /var/lib/apt/lists/* \
    && useradd -m -u 1000 server
COPY --chown=server:server . /server
COPY --chown=server:server --chmod=755 entrypoint.sh /server/entrypoint.sh
USER server
WORKDIR /server
# Unreal's UDP game port; PLAYSERV_ROOM_LISTEN_PORT overrides it when the platform allots one.
EXPOSE 7777/udp
ENTRYPOINT ["/server/entrypoint.sh"]
