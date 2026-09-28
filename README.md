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
| One world, three servers | A server claims a free region (0, 1, 2) in `WorldRegion`; the region gives its colour (red, blue, green), its room is `<colour>-<machine>` and its stretch of floor takes that colour. |
| Seamless crossing | When a player walks over a region border, the client joins the next server's room in the background and switches sockets once it answers; position and view are kept. |
| Players enter through the platform | `POST /auth/players/anon` → `GET /rooms/{slug}:browse` → `POST /rooms/{slug}/{room}:join` → WebSocket to the machine the reservation names. |
| Data a studio can read and edit | `WorldCube` and `CubeInventory` tables; `query_records` shows the world. |
| Serverless logic beside the servers | `CubeWorld.Refill`: a scheduled cloud function, `* * * * *`. `CubeWorld.Drop`: a cron every minute; a fire runs for half an hour and drops a bomb on every quarter minute (`timeout_s` 1860), the fires in between are skipped. It writes `WorldBomb` and every server hears it. |
| One object, one owner at a time | A free bomb is picked up only by the server of the region it lies in; a thrown one is flown by its thrower's server. A bomb only moves forward (free → held → flying → exploded, or free → fizzled), so a stale or echoed update is dropped. |

## Folder

| Path | What |
|---|---|
| `CubeWorld.Server/Spec.cs` | The Minecraft numbers and the block registry (hardness, drops, gravity). |
| `CubeWorld.Server/World.cs` | The tables and the world's rules: superflat terrain, place against a face, break, falling sand, apply a change from elsewhere, the inventory. |
| `CubeWorld.Server/CubeWorldServer.cs` | The game server: loads the world, keeps its room open, ticks 20 times a second, times digs, deals damage, writes and hears changes. |
| `CubeWorld.Server/Bomb.cs` | The `WorldBomb` table and how a bomb moves: the parachute, the pickup reach, the throw. |
| `CubeWorld.Refill/` | The refill function. |
| `CubeWorld.Drop/` | The bomb drop function: at most five free bombs, the oldest fizzles out for a new one. |
| `CubeWorld.Tests/` | The world's rules. |
| `web/` | The browser client, a static page; `bombs.js` flies bombs as the server does, `bombfx.js` draws them. |

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

## The Unreal client

`unreal/CubeWorld` is a UE 5.8 C++ project that plays on the same servers as the browser client:
sign-in, the room list and the join ticket go through the PlayServ Unreal SDK (the plugin under
`Plugins/PlayServSDK`, copied from `playserv-platform/unreal`), and the game socket speaks the
browser client's JSON frames, so a player in Unreal and a player in a browser share one world.
The world, the physics, the textures and the player model are ports of the browser client's
(`Source/CubeWorld/CubePhysics.cpp`, `CubeVoxelWorld.cpp`, `CubeTextures.cpp`, `CubeAvatar.cpp`);
`CubeSocket.cpp` is a small WebSocket client over the engine's TCP socket, because the engine's
own client asks the server for `//` and is refused.

Needs Unreal Engine 5.8 (`D:\EpicGames\UE_5.8`) and Visual Studio 2022 or later with the C++ workload.
The platform settings are in `Config/DefaultGame.ini` (`BaseURL`, the public `pk_` client key, the
room type `cubeworld`).

Build the editor target, create the material assets once, then run or package:

```bash
D:\EpicGames\UE_5.8\UE_5.8\Engine\Build\BatchFiles\Build.bat CubeWorldEditor Win64 Development -Project="<repo>\unreal\CubeWorld\CubeWorld.uproject" -WaitMutex -NoHotReload
D:\EpicGames\UE_5.8\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe "<repo>\unreal\CubeWorld\CubeWorld.uproject" -run=pythonscript -script="<repo>\unreal\CubeWorld\Scripts\MakeAssets.py" -unattended -nopause -nosplash
D:\EpicGames\UE_5.8\UE_5.8\Engine\Binaries\Win64\UnrealEditor.exe "<repo>\unreal\CubeWorld\CubeWorld.uproject" -game -windowed -resx=1280 -resy=720 -name=YourName
D:\EpicGames\UE_5.8\UE_5.8\Engine\Build\BatchFiles\RunUAT.bat BuildCookRun -project="<repo>\unreal\CubeWorld\CubeWorld.uproject" -platform=Win64 -clientconfig=Development -build -cook -stage -pak -archive -archivedirectory="<repo>\unreal\CubeWorld\Saved\Packaged"
```

The package lands in `Saved/Packaged/Windows/CubeWorld.exe`; give the whole `Windows` folder to whoever
wants to play. Keys: WASD, mouse, Space, Shift sprints, Ctrl sneaks, hold the left button to break,
the right places, 1-9 or the wheel pick a block, Enter plays, Esc frees the mouse. Command-line flags
for unattended runs: `-name=`, `-autoplay`, `-screenshot=<seconds>`, `-quitafter=<seconds>`,
`-selftest`, `-frametest`, `-logframes`, `-debughud`.
