# Cube World on PlayServ

A 3D block world of three regions side by side, one per game server. Players walk, place
and break cubes, see each other across the whole world, and walk from one server's region
into the next without a loading screen. Several copies of one C# game server run on
platform machines; none of them owns the world. The cubes live in platform
data, and every server hears every change the moment another server writes it. A cloud
function tops up each player's blocks once a minute; another drops two bombs on parachutes over every
room's region once a minute, which a player picks up, throws, and blows a crater with.

## What it shows

| Platform feature | Where |
|---|---|
| Game servers on platform machines | `set_machine_pool`: three Vultr machines, one server process each, one room per machine. |
| Servers share state through the platform | `Platform.RuntimeData.Write` / `Delete` on each change, `Platform.RuntimeData.Subscribe` + `Platform.OnRuntimeDataUpdate` to hear the other servers. |
| Players on every server see each other | Each server writes its players' positions to `WorldPresence` up to 20 times a second, when they change, and hears the other servers' players through the same subscription. |
| One world, six servers | The world is 72 × 48 blocks, six regions of 24 × 24 laid out like the six of a die (red, blue, green above; yellow, purple, pink below). No region belongs to a kind of server: any server, C# or Unreal, claims any free region in `WorldRegion` and keeps it while it lives, and a claim whose holder has not been seen for 30 s is free again. `WorldRegion` and `list_game_sessions` say which server holds which region now. The region gives the server its colour, its room is `<colour>-<machine>` and its square of floor takes that colour. The claim names the room type the room is registered under (`slug`: `cubeworld` for the C# pool, `cubeworld-ue` for the Unreal pool), so a client joins a neighbour under the right type. |
| Seamless crossing | When a player walks over a region border, the client joins the next server's room in the background and switches sockets once it answers; position and view are kept. |
| Players enter through the platform | `POST /auth/players/anon` → `GET /rooms/{slug}:browse` → `POST /rooms/{slug}/{room}:join` → WebSocket to the machine the reservation names. |
| Data a studio can read and edit | `WorldCube` and `CubeInventory` tables; `query_records` shows the world. |
| Serverless logic beside the servers | `CubeWorld.Refill`: a scheduled cloud function, `* * * * *`. `CubeWorld.Drop`: a cron every minute; each fire drops two bombs over every region a server holds (one room each) and ends at once. It writes `WorldBomb` and every server hears it. `CubeWorld.Reset`: called by URL with the server key, `POST /fn/cubeworld-reset`, it puts the whole world back to its default state, deleting every changed block, the three regions at once and 16 blocks at a time in each; every server and client hears the deletes. |
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
| `CubeWorld.Drop/` | The bomb drop function: two bombs a minute over every room's region, at most three free in a region, the oldest fizzles out for a new one; finished bombs are swept after two minutes. |
| `CubeWorld.Tests/` | The world's rules. |
| `deploy/web/` | The client's image for the web VM: Caddy serving `web/`. |
| `infra/vultr/` | Terraform for the web VM (Vultr, Caddy in Docker), and its guide. |
| `infra/gcp/` | Terraform for the client on Cloud Run (deployed from `dev` and `main`, like Pages), and its guide. |
| `web/` | The browser client, a static page; `bombs.js` flies bombs as the server does, `bombfx.js` draws them as creeper heads, `rooms.js` decides what to tell the player and when to try again after an operator's close or removal. |

## Build and test

```bash
dotnet test --solution CubeWorld.slnx
```

`global.json` runs `dotnet test` on Microsoft.Testing.Platform, which the xunit v3 tests need on the .NET 10 SDK
(`dotnet run --project CubeWorld.Tests` runs them too).

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

A push to `dev` or `main` that changes the client publishes it to GitHub Pages (`.github/workflows/pages.yml`) and
to Cloud Run (`.github/workflows/web-cloudrun.yml`; Terraform and guide in `infra/gcp`). A Vultr VM behind
Caddy (`web-image.yml` / `web-files.yml`; Terraform and guide in `infra/vultr`) is set up but paused: it deploys
only by hand until its `push:` triggers are uncommented.

## The Unreal dedicated server and client

`unreal/CubeWorld` is a UE 5.8 C++ project with the same game on Unreal's own dedicated server: the
servers, the regions, the shared world through platform data and the seamless crossing are
the C# server's, ported (`Source/CubeWorld/CubeWorldGameMode.cpp` is `CubeWorldServer.cs`,
`CubeServerWorld.cpp` is `World.cs`), and the client replicates with the server over **Iris**, Unreal's
replication system (`net.Iris.UseIrisReplication=1` in `Config/DefaultEngine.ini`,
`SetupIrisSupport` in the module's `Build.cs`). The Unreal servers play in `dev` beside the C# servers, as
the second room type `cubeworld-ue`, and claim free regions of the world exactly as the C# servers do; the two kinds of server
share the one world: the Unreal server also opens a WebSocket door that speaks the C# server's JSON protocol, and
the Unreal client speaks either protocol, by the room it enters. Both clients list both room types and join a
room under the type its region claim names.

| Piece | Where |
|---|---|
| The server signs in, loads the world, claims a region, opens its room | `CubeWorldGameMode.cpp`: `StartServer` → `LoadWorld` → `ClaimRegion` → `OpenRoom` (`PlayServ::Rooms::StartHosting` + `StartRoom`, or `StartRoomPlayServHosted` when the platform started the process) |
| Players are admitted by the platform's ticket | `PreLogin` → `PlayServ::Rooms::VerifyTicket`; the SDK reports the roster to the platform by itself |
| The rules: reach, digging by hardness, damage, knockback, bombs, explosions | `CubeWorldGameMode.cpp`, `CubeServerWorld.cpp`; the numbers in `CubeSpec.h` |
| Servers share the world through the platform | the SDK entity classes in `CubeEntities.h` (`WorldCube`, `WorldPresence`, `WorldHit`, `WorldRegion`, `WorldBomb`, `CubeInventory`) for the writes; the other servers' and the functions' writes arrive over the uplink, as on the C# servers (`CubeWorldGameMode_Live.cpp`): `SubscribeData` (the SDK's `subscribe_data`) for every shared table, keyed as the C# servers key them (`field:key`, `field:player_id`, `field:hit_id`, `field:bomb_id`), on which the platform sends every upsert and every delete, the reset function's deletes too. A deleted block goes back to the generated terrain on every client (kind index 255 over Iris, `op: "delete"` through the WebSocket door). The platform does not send again what changed while a subscription was not in place, so 3 s after every subscription goes out (the platform acknowledges none) the server reads the tables again. `WorldRegion` is read every 5 s in the heartbeat, as the C# servers read it. There are no windows on a timestamp and no table polls |
| What every client sees, over Iris | `CubeWorldState.cpp`: the presence list and the regions as replicated properties; block batches, digs, hits, deaths and bombs as multicast RPCs |
| What one player sends and gets | `CubePlayerPawn.cpp`: server RPCs for moves (client-simulated, as Minecraft's), digs, placements, hits, throws; client RPCs for the welcome, the world in chunks, the inventory |
| The client's session and the crossing | `CubeWorldGameInstance.cpp`: sign-in, `Browse`, `JoinRoom` with the ticket; an Unreal room (attribute `engine=unreal`) is entered with `ClientTravel` over Iris, a C# room over the JSON socket (`CubeSocket.cpp`, `CubeWorldGameInstance_Socket.cpp`), so one client walks between both kinds of server; a border crossing hands the next server the position and keeps the world: the client runs its own engine (`CubeGameEngine.cpp`, `GameEngine=` in `Config/DefaultEngine.ini`), which plays on with the old server until the next one has let the player in and then gives the next server's connection the same world, so no map is loaded and nothing is drawn again. A socket to a C# server is opened while the player still plays on the Unreal one, and the other way round; a handshake that fails, or has not let the player in after 10 s, leaves them where they are, still playing (`-noseamless` brings back the old travel through the local map) |
| A second door on the Unreal server, for browsers | `CubeWebSocketServer.cpp` listens ten ports above the game port (`-wsport=`, or `CUBEWORLD_WS_PORT`) and `CubeWorldGameMode_Web.cpp` speaks the C# server's JSON frames on it, admitting a player by the same platform ticket; the room's attribute `ws` names the door (`-wsaddress=` or `CUBEWORLD_WS_ADDRESS` when the machine's public address differs), and the browser client (`web/app.js`) takes it from the join ticket. Every change the server replicates over Iris is also sent as a JSON frame from the same place |

Needs Unreal Engine 5.8 from the Epic Games Launcher (`<engine>` below is its install folder, the one that holds
`Engine\`, for example `D:\EpicGames\UE_5.8`; the scripts find it through the Launcher's own record, or take
`-Engine <folder>`) and Visual Studio 2022 or later with the C++ workload.
The platform settings are in `Config/DefaultGame.ini` (`BaseURL`, the public `pk_` client key of environment
`dev`, the room type `cubeworld-ue`); the server's `sk_` key goes in `Config/DedicatedServerGame.ini`, which
git ignores (copy `DedicatedServerGame.example.ini`).

**The Launcher's engine cannot build a Server target** (it ships no `UnrealServer` binaries), so on a
developer machine the server is the editor run headless with `-server`. Build the editor target, create the
material assets once, then start the servers and a client:

```bash
<engine>\Engine\Build\BatchFiles\Build.bat CubeWorldEditor Win64 Development -Project="<repo>\unreal\CubeWorld\CubeWorld.uproject" -WaitMutex -NoHotReload
<engine>\Engine\Binaries\Win64\UnrealEditor-Cmd.exe "<repo>\unreal\CubeWorld\CubeWorld.uproject" -run=pythonscript -script="<repo>\unreal\CubeWorld\Scripts\MakeAssets.py" -unattended -nopause -nosplash
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
frame drawn around a crossing, and `python Scripts/AnalyzeCrossings.py` reads that log: every crossing, the frames
around it (the largest step of the view and gap between two frames), any map loaded or chunks rebuilt.
`-DoorRegions 4` plays region 4 through its server's JSON door, as a C# server is played, so crossings between Iris
and a socket are tried as well.

**Packaging the client** for others to run is UAT's BuildCookRun, in a checkout of its own:

- **Windows**, on Windows: `<engine>\Engine\Build\BatchFiles\RunUAT.bat BuildCookRun -project=<repo>\unreal\CubeWorld\CubeWorld.uproject -platform=Win64 -clientconfig=Development -build -cook -stage -pak -archive -archivedirectory=<repo>\unreal\CubeWorld\Saved\Packaged -unattended -utf8output -nop4`. The game is `Saved\Packaged\Windows\CubeWorld.exe`.
- **Mac**, on a Mac: `<engine>/Engine/Build/BatchFiles/RunUAT.sh BuildCookRun -project=<repo>/unreal/CubeWorld/CubeWorld.uproject -platform=Mac -clientconfig=Development -build -cook -stage -pak -package -archive -archivedirectory=<dir> -unattended -utf8output -nop4`. The game is `<dir>/Mac/CubeWorld.app`.
  - **`-package` is required on Mac.** The archive step copies `Binaries/Mac/CubeWorld.app`, and only the package step (Xcode) puts the staged game into it. Without `-package` the archived app has no content and no `libtbb`, and dies at launch (PSV-2988). Unarchived, `Saved/StagedBuilds/Mac/CubeWorld.app` runs as well.
  - `Saved/StagedBuilds/Mac/Manifest_UFSFiles_Mac.txt` must list `cacert.pem`. The Mac build has no other root certificates, and without them it cannot reach a C# room (`Config/Mac/MacEngine.ini`, PSV-2986).
  - A packaged Mac game logs to `~/Library/Containers/com.YourCompany.CubeWorld/Data/Library/Logs/CubeWorld/CubeWorld.log`, not `Saved/Logs`.
  - On a Mac, a key held through a border crossing stops working until it is pressed again: the game reads the keyboard itself only on Windows (`CubeKeys`).

Putting the server on the platform's machine pool takes the Server target, `CubeWorldServer`, built for Linux. The
Launcher's engine has no Server target, so that build takes an engine built from source (here
`C:\PlayServ\UnrealEngine`); the clients stay on the Launcher's. `RUNBOOK.md`, "Part E".
