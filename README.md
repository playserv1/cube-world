# Cube World on PlayServ

A 3D block world of three regions side by side, one per game server. Players walk, place
and break cubes, see each other across the whole world, and walk from one server's region
into the next without a loading screen. Several copies of one C# game server run on
platform machines; none of them owns the world. The cubes live in platform
data, and every server hears every change the moment another server writes it. A cloud
function refills each player's cubes once a minute.

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
| Serverless logic beside the servers | `CubeWorld.Refill`: a scheduled cloud function, `* * * * *`. |

## Folder

| Path | What |
|---|---|
| `CubeWorld.Server/World.cs` | The two tables and the world's rules: stack, break, apply a change from elsewhere. |
| `CubeWorld.Server/CubeWorldServer.cs` | The game server: loads the world, keeps its room open, answers players, writes and hears changes. |
| `CubeWorld.Refill/` | The refill function. |
| `CubeWorld.Tests/` | The world's rules. |
| `web/` | The browser client, a static page. |

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

Deploying the servers and the function is in `RUNBOOK.md`.
