# Cube World — deploy and live demo

Everything below runs against the dev platform, `https://dev.platform.playserv.io`.
`agent (MCP)` is a tool of the `playserv` MCP connector, `shell` is a terminal.

## Part A — the project (once)

| # | who | step |
|---|---|---|
| A1 | agent (MCP) | `create_project(name="Cube World", region="fra")` → `$PROJECT` |
| A2 | agent (MCP) | `apply_schema_state` with six entities: `WorldCube` (`key` text **primary**, `x` `y` `z` integer, `kind` text, `placed_by` text, `placed_on` text), `CubeInventory` (`player_id` text **primary**, `cubes` integer indexed, `stacks` text), `WorldPresence` (`player_id` text **primary**, `name` `server` `color` text, `x` `y` `z` `yaw` `pitch` `health` decimal, `sneaking` `sprinting` `seen_at` integer), `WorldHit` (`hit_id` text **primary**, `victim` `attacker` text, `damage` `kx` `ky` `strength` decimal, `at` integer), `WorldBomb` (`bomb_id` text **primary**, `state` `holder` text, `x` `y` `z` `vx` `vy` `vz` decimal, `dropped_at` `at` integer) and `WorldRegion` (`region` text **primary**, `server` `color` `room` text, `seen_at` integer) |
| A3 | agent (MCP) | `create_api_key(type="server")` → `$SK`; `create_api_key(type="client")` → `$PK` |
| A4 | agent (MCP) | `create_function(name="cubeworld", slug=$SLUG, runtime="dotnet10", kind="game_server", hosting_mode="multi-room")` → `$FN` |
| A5 | agent (MCP) | `set_room_configuration(function_id=$FN, capacity=16, reservation_ttl_seconds=20, room_lifetime_seconds=86400, room_idle_timeout_seconds=60, max_rooms=20)` |

`$SLUG` must be unique across the organisation: `cubeworld-<suffix of $PROJECT>`.

## Part B — the refill and drop functions

```bash
playserv login $SK
playserv functions deploy --slug cubeworld-refill-<suffix> --kind cloud_function --src CubeWorld.Refill
playserv functions deploy --slug cubeworld-drop-<suffix> --kind cloud_function --src CubeWorld.Drop
```

Live in about 150 s. Both crons in `platform.json` fire every minute; each drop fire runs 45 s and drops four bombs.

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

## Tear-down

agent (MCP): `destroy_machine_pool(executor_slug=$SLUG)` — it closes every room and destroys every machine — then delete the project. `remove_pool_machine` takes out one machine and lowers `desired_size` by one.
