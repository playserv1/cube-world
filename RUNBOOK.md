# Cube World — deploy and live demo

Everything below runs against the dev platform, `https://dev.platform.playserv.io`.
`agent (MCP)` is a tool of the `playserv` MCP connector, `shell` is a terminal.

## Part A — the project (once)

| # | who | step |
|---|---|---|
| A1 | agent (MCP) | `create_project(name="Cube World", region="fra")` → `$PROJECT` |
| A2 | agent (MCP) | `apply_schema_state` with six entities: `WorldCube` (`key` text **primary**, `x` `y` `z` integer, `kind` text, `placed_by` text, `placed_on` text), `CubeInventory` (`player_id` text **primary**, `cubes` integer indexed, `stacks` text), `WorldPresence` (`player_id` text **primary**, `name` `server` `color` text, `x` `y` `z` `yaw` `pitch` `health` decimal, `sneaking` `sprinting` `seen_at` integer), `WorldHit` (`hit_id` text **primary**, `victim` `attacker` text, `damage` `kx` `ky` `strength` decimal, `at` integer), `WorldBomb` (`bomb_id` text required indexed — not primary and not unique, a bomb may have several rows; `state` `holder` text, `x` `y` `z` `vx` `vy` `vz` decimal, `dropped_at` `at` integer) and `WorldRegion` (`region` text **primary**, `server` `color` `room` text, `seen_at` integer) |
| A3 | agent (MCP) | `create_api_key(type="server")` → `$SK`; `create_api_key(type="client")` → `$PK` |
| A4 | agent (MCP) | `create_function(name="cubeworld", slug=$SLUG, runtime="dotnet10", kind="game_server", hosting_mode="multi-room")` → `$FN` |
| A5 | agent (MCP) | `set_room_configuration(function_id=$FN, capacity=16, reservation_ttl_seconds=20, room_lifetime_seconds=2592000, max_rooms=20)` — no `room_idle_timeout_seconds`: a region's room must stay open while nobody is in it, or a player walking in is refused `409 room_closed` for up to three minutes (the SDK drops an empty room after the idle timeout, the platform marks it closing after idle + 120 s, and the room the server opens again under the same name inherits that). The platform wants one expiry at least, so the lifetime is 30 days; the server opens its room again when it ends. |

`$SLUG` must be unique across the organisation: `cubeworld-<suffix of $PROJECT>`.

## Part B — the refill, drop and reset functions

```bash
playserv login $SK
playserv functions deploy --slug cubeworld-refill-<suffix> --kind cloud_function --src CubeWorld.Refill
playserv functions deploy --slug cubeworld-drop-<suffix> --kind cloud_function --src CubeWorld.Drop
playserv functions deploy --slug cubeworld-reset-<suffix> --kind cloud_function --src CubeWorld.Reset
```

Live in about 150 s. Both crons in `platform.json` fire every minute. A drop fire drops four bombs, 15 s apart, and ends. The reset has no trigger and takes the server key (a player's `pk_` is refused `403`): `curl -X POST $PLAYSERV_HOST/fn/cubeworld-reset-<suffix> -H "Authorization: Bearer $SK" -d '{}'` puts the world back to its default state and answers `{"ok":true,"deleted":N,"regions":{"0":…,"1":…,"2":…}}`.

## Part C — the servers on Vultr

Needs Docker with `buildx` (Docker Desktop on macOS and Windows) and the CLI session from Part B.

1. Build and push the image, from the repository root:

   ```bash
   playserv image push --slug $SLUG --src CubeWorld.Server --tag 1.0.0
   ```

   The CLI writes the Dockerfile for the C# game server, builds it for `linux/amd64` whatever the
   machine you run it on, logs Docker in to the project's registry and pushes
   `<registry>/<repository>/$SLUG:1.0.0`. On a new project the first registry credential can take a
   few minutes while Google IAM propagates; the command waits. The image carries no key: on a pool
   machine the server verifies its players with the machine's own deployment token.

2. agent (MCP): `set_machine_pool(executor_slug=$SLUG, desired_size=3, rooms_per_machine=1, image_version="1.0.0")`.
   `list_pool_machines` shows each machine go `booting → ready → in_rotation`; each server
   then claims a free region in `WorldRegion` (0 red, 1 blue, 2 green) and opens its room
   `<colour>-<last 5 chars of the machine id>`, e.g. `red-d0q2w`. A fourth server waits until a
   region frees up (its holder stops refreshing `seen_at` for 30 s).

3. A new version, once the pool exists, is one command:

   ```bash
   playserv image push --slug $SLUG --src CubeWorld.Server --tag 1.0.1 --roll
   ```

   `--roll` sets the pool's `image_version` to the tag and keeps its size. **The pool replaces its
   machines one for one.** Each old machine is closed — its room and players with it — and destroyed
   as soon as a new machine is in rotation to take its place. The new server takes the freed region
   once the old holder has stopped refreshing it for 30 s, so a region is empty for about half a
   minute. Do not roll out during the demo.

**Raise the pool before the demo, not during it.**

## Part D — the live script

| # | what the audience sees | how |
|---|---|---|
| D1 | Three servers on three machines | `list_pool_machines`, then the client's server list |
| D2 | Three browser tabs, one per server | open `http://localhost:5173` three times, type a name, **Play**, then **enter** a different server in each; the floor takes the server's colour |
| D3 | One world, three coloured regions, a player walks across | in tab 1 click the world, walk (WASD) from red to blue: the banner switches server, nothing reloads; tabs 2 and 3 show the figure all the way |
| D4a | A cube placed on server 1 appears on servers 2 and 3, outlined white | place in tab 1, watch tabs 2 and 3 |
| D4b | The player names are in the admin | the room's participants, each player's `name` |
| D4c | The world is platform data | `query_records(entity="WorldCube")` |
| D5 | Leave a server, enter another: the world and your cubes are still there | press **enter** on another server |
| D6 | Out of cubes, then the refill arrives | place until the bar is empty; within a minute the function tops it up; `list_function_logs` of the refill shows `refilled N inventories` |
| D7 | A bomb comes down on a parachute, a player picks it up and throws it across a border | walk into a bomb in tab 1: it is in the hand in tab 1 and in the figure's hand in tabs 2 and 3; right click throws it; the crater and the damage show on every server; `list_function_logs` of the drop shows `dropped bomb` every 15 s |
| D8 | Never more than five bombs | leave the bombs lying: the sixth drop makes the oldest go up in a puff of smoke |
| D9 | The operator removes a player | admin → the room → **Remove player** (or `remove_room_participant`): that tab says an operator removed it; walking back into that region is refused, the other regions still let it in |
| D10 | The operator closes a room, and it comes back | build something in a region, then admin → **Delete room** (or `close_room`): its tabs say the room was closed; within a minute or two the room is back under the same name, empty, and what was built there is still there. `list_function_logs` of the server shows `was closed: restarting` |
| D11 | The world goes back to its default state | build and dig in every region, then `curl -X POST $PLAYSERV_HOST/fn/cubeworld-reset-<suffix> -H "Authorization: Bearer $SK" -d '{}'`: in every tab the changed blocks of all three regions disappear a few at a time, together, until the terrain and the oaks are as generated; the answer counts the deleted blocks, in all and per region |

To take a region away for good, remove its machine: `remove_pool_machine` closes its room, destroys the
machine and lowers the pool's size by one, so no replacement is requested. Closing the room alone brings it
back, because the server's process restarts on the same machine.


## Part E — the Unreal dedicated server

The Unreal servers (`unreal/CubeWorld`, README "The Unreal dedicated server and client") play in `dev` beside
the C# servers, as a second game server `cubeworld-ue` with its own pool: the world is six regions, the C#
pool takes the upper row and the Unreal pool the lower, so they never race for a region while both rows are
up. What `dev` got for it, and how (all through the agent's MCP tools; the `ue` environment, a copy of `dev`
from 2026-09-29, remains for Unreal-only tests):

| Step | How |
|---|---|
| The room type | `create_function(slug="cubeworld-ue", kind="game_server", hosting_mode="multi-room")`; `set_room_configuration(capacity=16, reservation_ttl=20, room_lifetime=2592000, max_rooms=10)`, no idle close. A server with an `sk_` key registers rooms under it by itself (`RoomDefaultSlug` in `Config/DefaultGame.ini`, or `PLAYSERV_EXECUTOR_SLUG` from the platform) |
| The schema | `WorldRegion` got `slug` (string): the room type of the room the claim names, so the clients join a neighbour under the right type. `WorldCube` got `at` (integer, indexed), the time of the last change, which the servers' live subscriptions and fallback polls window by |
| The keys | a client key for `Config/DefaultGame.ini` and a server key for each developer's `Config/DedicatedServerGame.ini` (dashboard → API keys → environment `dev`; never in git) |
| Local servers | `set_env_local_development(env_id=<dev>, true)`, so a server on a developer's machine may register a private address |

**On a developer machine** the servers are the editor run headless (`Scripts/RunServers.ps1`); the platform
lists their rooms (`list_game_sessions(env="dev")`) beside the C# servers', and the admin's **Remove player**
and **Delete room** work the same way: the room's server turns the players away, clears its region and exits
(start it again by hand, since no Docker restarts it there).

**On the platform's machine pool** the server is a Linux image. The Launcher's engine has no Server target, so
the image is the Game target run headless as a listen server (`-cubeserver` in `Docker/entrypoint.sh`; the
process is a server in every way but the spectator it keeps for itself; the SDK logs one error line at start,
that a server credential is set in a process that is not a dedicated server, and then serves with it; the
`DedicatedServerGame.ini` layer is not read in this mode, so the key comes from `PLAYSERV_SERVER_KEY` or, on
the pool, the deployment token). It needs, once, in the Epic Games
Launcher (Library → Engine Versions → 5.8 → Options), the **Linux** target platform ticked, and the Linux
cross-toolchain for 5.8 installed (`v26_clang-20.1.8-rockylinux8`, the installer sets `LINUX_MULTIARCH_ROOT`).

1. Build and package: `RunUAT.bat BuildCookRun -project=<repo>\unreal\CubeWorld\CubeWorld.uproject -platform=Linux -clientconfig=Development -build -cook -stage -pak -archive -archivedirectory=<out>`.
2. Push the image: `playserv image push --slug cubeworld-ue --src <out>\Linux --dockerfile <repo>\unreal\CubeWorld\Docker\Dockerfile --tag ue-1.0.0` (logged in with an `sk_` key of environment `dev`).
3. The pool: `set_machine_pool(env="dev", executor_slug="cubeworld-ue", desired_size=3, rooms_per_machine=1, image_version="ue-1.0.0")`. One process per machine, as the C# pool; each claims a region of the lower row and opens `<colour>-<machine>`. The process reads `PLAYSERV_DEPLOYMENT_TOKEN` for its credential and listens on the port the platform allots it (`PLAYSERV_ROOM_LISTEN_PORT`, 7777 when unset): Iris on UDP and the JSON door on TCP, both on that number (the image runs with host networking). Browsers never reach the door directly: the platform's TLS front offers it as `wss` on the public port that `PLAYSERV_PORTS_MAPPING` names (protocol `wss`), under the machine's name (`PLAYSERV_PUBLIC_HOST`, the certificate is for `*.pool.dev.playserv.com`), and the room registers that front as its connect, as the C# rooms do. The UDP address for Iris rides in the room's attribute `udp`, and the Unreal client dials it by the machine's address (`playserv_public_ip`), not its name.
4. Taking the Unreal pool down (`destroy_machine_pool(executor_slug="cubeworld-ue")`, or `remove_pool_machine` one at a time) leaves the C# row playing: the rooms vanish from the browse within about fifteen seconds, the claims expire after thirty, and a C# server that restarts may then take a lower region.

With a source-built engine the Server target works instead (`-server -serverplatform=Linux -noclient`, the
`CubeWorldServer` binary); `entrypoint.sh` runs whichever binary the archive holds.

## Tear-down

agent (MCP): `destroy_machine_pool(executor_slug=$SLUG)` — it closes every room and destroys every machine — then delete the project. `remove_pool_machine` takes out one machine and lowers `desired_size` by one.
