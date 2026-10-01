# Cube World on PlayServ

A 3D block world of three regions side by side, one per game server. Players walk, place
and break cubes, see each other across the whole world, and walk from one server's region
into the next without a loading screen. Several copies of one C# game server run on
platform machines; none of them owns the world. The cubes live in platform
data, and every server hears every change the moment another server writes it. A cloud
function tops up each player's blocks once a minute; another drops a bomb on a parachute every
15 seconds, which a player picks up, throws, and blows a crater with.

## What it shows

| Platform feature | Where |
|---|---|
| Game servers on platform machines | `set_machine_pool`: three Vultr machines, one server process each, one room per machine. |
| Servers share state through the platform | `Platform.RuntimeData.Write` / `Delete` on each change, `Platform.RuntimeData.Subscribe` + `Platform.OnRuntimeDataUpdate` to hear the other servers. |
| Players on every server see each other | Each server writes its players' positions to `WorldPresence` five times a second and hears the other servers' players through the same subscription. |
| One world, six servers | The world is 72 × 48 blocks, six regions of 24 × 24 laid out like the six of a die: the upper row (0, 1, 2) is the C# servers', the lower (3, 4, 5) the Unreal servers'. A server claims a free region of its own row first in `WorldRegion` (any free one when its row is full); the region gives its colour (red, blue, green above; yellow, purple, pink below), its room is `<colour>-<machine>` and its square of floor takes that colour. The claim names the room type the room is registered under (`slug`: `cubeworld` for the C# pool, `cubeworld-ue` for the Unreal pool), so a client joins a neighbour under the right type. |
| Seamless crossing | When a player walks over a region border, the client joins the next server's room in the background and switches sockets once it answers; position and view are kept. |
| Players enter through the platform | `POST /auth/players/anon` → `GET /rooms/{slug}:browse` → `POST /rooms/{slug}/{room}:join` → WebSocket to the machine the reservation names. |
| Data a studio can read and edit | `WorldCube` and `CubeInventory` tables; `query_records` shows the world. |
| Serverless logic beside the servers | `CubeWorld.Refill`: a scheduled cloud function, `* * * * *`. `CubeWorld.Drop`: a cron every minute; each fire drops four bombs, 15 seconds apart. It writes `WorldBomb` and every server hears it. `CubeWorld.Reset`: called by URL with the server key, `POST /fn/cubeworld-reset`, it puts the whole world back to its default state, deleting every changed block, the three regions at once and 16 blocks at a time in each; every server and client hears the deletes. |
| An operator removes a player or closes a room | The admin's **Remove player** / **Delete room** (MCP `remove_room_participant` / `close_room`). A removed player's socket closes `1008 removed_by_operator` and the room refuses them `409 removed_from_room` while it lives; they can still walk into the other regions. A closed room's players get `1008 room_closed_by_operator`; the bombs over its region fizzle, the server ends its process, and Docker starts it again on the same machine: the room opens fresh under the same name, empty, within a minute or two. |
| One object, one owner at a time | A free bomb is picked up only by the server of the region it lies in; a thrown one is flown by its thrower's server. A bomb only moves forward (free → held → flying → exploded, or free → fizzled), so a stale or echoed update is dropped. A blast breaks each region's blocks on that region's server alone: every server hears the bomb go off and works the blast out from the same centre and seed, so the shares meet in one crater; a region whose server is not up at that moment keeps its blocks. |

## Folder

| Path | What |
|---|---|
| `CubeWorld.Server/Spec.cs` | The Minecraft numbers and the block registry (hardness, drops, gravity). |
| `CubeWorld.Server/World.cs` | The tables and the world's rules: superflat terrain, place against a face, break, falling sand, apply a change from elsewhere, the inventory. |
| `CubeWorld.Server/CubeWorldServer.cs` | The game server's outline: how it starts, what one tick does, what a player can do, what it hears from the other servers. |
| `CubeWorld.Server/CubeWorldServer.Players.cs` | Joining, moving, digging, placing, fighting, dying and coming back. |
| `CubeWorld.Server/CubeWorldServer.Bombs.cs` | The parachute, the pickup, the throw, the blast. |
| `CubeWorld.Server/CubeWorldServer.Sharing.cs` | The region this server holds, the positions and blocks the servers share; when its room is closed, restarts. |
| `CubeWorld.Server/Bomb.cs` | The `WorldBomb` table and how a bomb moves: the parachute, the pickup reach, the throw. |
| `CubeWorld.Refill/` | The refill function. |
| `CubeWorld.Reset/` | The reset function: every changed block deleted, the three regions in parallel, so the world is the generated terrain again. |
| `CubeWorld.Drop/` | The bomb drop function: at most five free bombs, the oldest fizzles out for a new one; finished bombs are swept after two minutes. |
| `CubeWorld.Tests/` | The world's rules. |
| `web/` | The browser client, a static page; `bombs.js` flies bombs as the server does, `bombfx.js` draws them as creeper heads, `rooms.js` decides what to tell the player and when to try again after an operator's close or removal. |

## Build and test

```bash
dotnet test --solution CubeWorld.slnx
```

`PlayServ.Sdk` comes from the PlayServ NuGet feed (`nuget.config`), not from this repository.

## Run the client locally

```bash
cp web/config.example.js web/config.js
python3 -m http.server 5173 -d web
```

Fill `config.js` with the API URL, the project's client key and the game server's slug,
then open `http://localhost:5173` in several tabs and pick a different server in each.
`http://localhost:5173/?offline=1` runs the client alone, without the platform: a local world with
one other player, for looking at blocks, physics and the model.

Deploying the servers and the function is in `RUNBOOK.md`.

## The Unreal dedicated server and client

`unreal/CubeWorld` is a UE 5.8 C++ project with the same game on Unreal's own dedicated server: the
servers, the regions, the shared world through platform data and the seamless crossing are
the C# server's, ported (`Source/CubeWorld/CubeWorldGameMode.cpp` is `CubeWorldServer.cs`,
`CubeServerWorld.cpp` is `World.cs`), and the client replicates with the server over **Iris**, Unreal's
replication system (`net.Iris.UseIrisReplication=1` in `Config/DefaultEngine.ini`,
`SetupIrisSupport` in the module's `Build.cs`). The Unreal servers play in `dev` beside the C# servers, as
the second room type `cubeworld-ue`, and take the lower row of the world's six regions; the two kinds of server
share the one world: the Unreal server also opens a WebSocket door that speaks the C# server's JSON protocol, and
the Unreal client speaks either protocol, by the room it enters. Both clients list both room types and join a
room under the type its region claim names.

| Piece | Where |
|---|---|
| The server signs in, loads the world, claims a region, opens its room | `CubeWorldGameMode.cpp`: `StartServer` → `LoadWorld` → `ClaimRegion` → `OpenRoom` (`PlayServ::Rooms::StartHosting` + `StartRoom`, or `StartRoomPlayServHosted` when the platform started the process) |
| Players are admitted by the platform's ticket | `PreLogin` → `PlayServ::Rooms::VerifyTicket`; the SDK reports the roster to the platform by itself |
| The rules: reach, digging by hardness, damage, knockback, bombs, explosions | `CubeWorldGameMode.cpp`, `CubeServerWorld.cpp`; the numbers in `CubeSpec.h` |
| Servers share the world through the platform | the SDK entity classes in `CubeEntities.h` (`WorldCube`, `WorldPresence`, `WorldHit`, `WorldRegion`, `WorldBomb`, `CubeInventory`) for the writes; the other servers' writes arrive over the platform's realtime socket as collection subscriptions (`CubeLiveTables.cpp`, `CubeWorldGameMode_Live.cpp`): each table is watched through a window on its timestamp (`at` / `seen_at`), the platform pushes the matching rows the moment one changes, and a window that fills up moves forward. The SDK's own realtime client subscribes to single records only and signs the socket with the client key, which the platform refuses for a server, so the server speaks the SDK's wire itself with its server credential. Slow polls stay behind as the fallback while the socket is down. A window on `at` sees no deleted row, and the reset function deletes every changed block, so the blocks are also heard over the uplink, as the C# servers hear them: `SubscribeData` (the SDK's `subscribe_data`), on which the platform sends every upsert and every delete, the reset function's deletes too. A deleted block goes back to the generated terrain on every client (kind index 255 over Iris, `op: "delete"` through the WebSocket door), and on every new uplink socket the server reads the table again for what changed while it was not listening |
| What every client sees, over Iris | `CubeWorldState.cpp`: the presence list and the regions as replicated properties; block batches, digs, hits, deaths and bombs as multicast RPCs |
| What one player sends and gets | `CubePlayerPawn.cpp`: server RPCs for moves (client-simulated, as Minecraft's), digs, placements, hits, throws; client RPCs for the welcome, the world in chunks, the inventory |
| The client's session and the crossing | `CubeWorldGameInstance.cpp`: sign-in, `Browse`, `JoinRoom` with the ticket; an Unreal room (attribute `engine=unreal`) is entered with `ClientTravel` over Iris, a C# room over the JSON socket (`CubeSocket.cpp`, `CubeWorldGameInstance_Socket.cpp`), so one client walks between both kinds of server; a border crossing hands the next server the position |
| A second door on the Unreal server, for browsers | `CubeWebSocketServer.cpp` listens ten ports above the game port (`-wsport=`, or `CUBEWORLD_WS_PORT`) and `CubeWorldGameMode_Web.cpp` speaks the C# server's JSON frames on it, admitting a player by the same platform ticket; the room's attribute `ws` names the door (`-wsaddress=` or `CUBEWORLD_WS_ADDRESS` when the machine's public address differs), and the browser client (`web/app.js`) takes it from the join ticket. Every change the server replicates over Iris is also sent as a JSON frame from the same place |

Needs Unreal Engine 5.8 (`D:\EpicGames\UE_5.8`) and Visual Studio 2022 or later with the C++ workload.
The platform settings are in `Config/DefaultGame.ini` (`BaseURL`, the public `pk_` client key of environment
`dev`, the room type `cubeworld-ue`); the server's `sk_` key goes in `Config/DedicatedServerGame.ini`, which
git ignores (copy `DedicatedServerGame.example.ini`).

**The Launcher's engine cannot build a Server target** (it ships no `UnrealServer` binaries), so on a
developer machine the server is the editor run headless with `-server`. Build the editor target, create the
material assets once, then start the servers and a client:

```bash
D:\EpicGames\UE_5.8\UE_5.8\Engine\Build\BatchFiles\Build.bat CubeWorldEditor Win64 Development -Project="<repo>\unreal\CubeWorld\CubeWorld.uproject" -WaitMutex -NoHotReload
D:\EpicGames\UE_5.8\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe "<repo>\unreal\CubeWorld\CubeWorld.uproject" -run=pythonscript -script="<repo>\unreal\CubeWorld\Scripts\MakeAssets.py" -unattended -nopause -nosplash
powershell -File <repo>\unreal\CubeWorld\Scripts\RunServers.ps1          # alpha:7777, beta:7778, gamma:7779
powershell -File <repo>\unreal\CubeWorld\Scripts\RunClient.ps1 -Name Ann
```

Each server registers its room under this machine's address; the `dev` environment is flagged for local
development, so a private address is accepted. Players on another machine reach it only if that address
routes to it (a LAN, or `-PublicHost=<address>` with the UDP ports forwarded). The servers' logs are
`Saved/Logs/server-<name>.log`, a client's `Saved/Logs/client-<name>.log`. Keys: WASD, mouse, Space, Shift
sprints, Ctrl sneaks, hold the left button to break, the right places (or throws the bomb in the hand; walk
into a bomb to pick it up), 1-9 or the wheel pick a block, Enter plays, Esc frees the mouse. Command-line flags
for unattended runs: `-name=`, `-autoplay`, `-screenshot=<seconds>`, `-quitafter=<seconds>`, `-selftest`,
`-walkto=<x>` or `-walkto=<x>,<y>`, `-debughud`.

`Scripts/RunOffline.ps1` plays the game on this machine alone, with no platform at all (`-cubeoffline`): two or
three Unreal servers in regions 3, 4 and 5 (UDP 7777-7779) and, with `-Client -WalkTo 30`, a client that walks over
their borders (`-NullRhi -QuitAfter 40` for an unattended run, `-Stop` ends them all). Nothing signs in to `dev`,
which is shared and live: each server keeps its own world, checks no ticket and hands everyone the starting
inventory. The logs are `Saved/Logs/offline-<name>.log`; `-logcrossing` on the client (`-ClientExtra`) logs every
frame drawn around a crossing.

Putting the server on the platform's machine pool takes a Linux build. The Launcher's engine has no Server
target, so the image runs the Game target headless as a listen server (`-cubeserver`, `Docker/entrypoint.sh`):
no picture, no sound, its own local player a spectator that is not a player of the world. `RUNBOOK.md`, "Part E".
