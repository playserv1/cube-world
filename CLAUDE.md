# Cube World

## Key rule: the two clients change together

There are two game clients: the Unreal client (`unreal/CubeWorld`) and the web client (`web/`).

- **When game logic changes in one client, it changes in the other in the same piece of work.** Logic means what the player can do and what happens: controls, menus and what their buttons do, movement, crossings between servers, joining rooms, bombs, blocks, combat, inventory rules.
- **The HUD is similar, but adapted to each client.** It holds the same elements and states in both. Each client draws them in its own way, with sizes that read well there. For example, the Unreal HUD is drawn for a 480-pixel-high screen and scaled up to the real one.
- A difference is allowed only where the platform forces it. For example, Exit quits the Unreal game, but on the web it leaves for the start page. Note such a difference in the code.

## Key rule: a shared table is checked on both servers

The C# servers and the Unreal servers read and write the same platform tables (`WorldCube`, `WorldPresence`,
`WorldBomb`, `WorldHit`, `WorldRegion`, `CubeInventory`). Each side declares its own copy of every row:
`CubeWorld.Server/World.cs` and `Bomb.cs` (plus the copies in `CubeWorld.Drop`, `CubeWorld.Reset`, `CubeWorld.Refill`)
on the C# side, `unreal/CubeWorld/Source/CubeWorld/CubeEntities.h` on the Unreal side.

- **When a field of a shared table is added, renamed, retyped or starts being relied on, check it on both servers in the
  same piece of work**: that both sides declare it, with the same name and a type that reads what the other side
  writes, and that both sides write it.
- **Check the rows already in `dev`, not only the code.** A field one side added and the other never wrote comes back
  as `null` in every old row (`query_records` with an `is_null` filter finds them). A C# field that cannot take `null`
  (`long`, `double`, `int`) fails the whole load on the first such row, and the server never opens its room. Declare a
  field that old rows may lack as nullable (`double?`), and fill it in the old rows.
- **After the deploy, read the servers' logs** (`list_function_logs`) and check that rooms of both room types,
  `cubeworld` and `cubeworld-ue`, came up (`list_game_sessions`). A server that cannot read the world logs
  `world not ready` and keeps retrying; nothing else shows it.

This rule exists because it was broken on 2026-09-30: the Unreal servers had written `WorldCube.at` since 2026-09-29,
the C# servers had not, and when the C# servers started reading it as a `long`, and then as a `double`, 2569 old rows
with `at: null` kept every C# server from opening a room.

## Working rules

- Chat with the user in Ukrainian. Commits, docs, Jira and code comments are in English.
- Every change ends with a Windows build of the Unreal client in `unreal/CubeWorld/Saved/Packaged/Windows`, so the user can try it. If the change touches the Unreal servers, fix and deploy the servers first.
- Game servers and clients speak wss, never plain ws. The Unreal servers replicate over Iris.
- Server keys never go into git or into an image. `Config/DedicatedServerGame.ini` is gitignored.
