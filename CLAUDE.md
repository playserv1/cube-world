# Cube World

## Key rule: the two clients change together

There are two game clients: the Unreal client (`unreal/CubeWorld`) and the web client (`web/`).

- **When game logic changes in one client, it changes in the other in the same piece of work.** Logic means what the player can do and what happens: controls, menus and what their buttons do, movement, crossings between servers, joining rooms, bombs, blocks, combat, inventory rules.
- **The HUD is similar, but adapted to each client.** It holds the same elements and states in both. Each client draws them in its own way, with sizes that read well there. For example, the Unreal HUD is drawn for a 480-pixel-high screen and scaled up to the real one.
- A difference is allowed only where the platform forces it. For example, Exit quits the Unreal game, but on the web it leaves for the start page. Note such a difference in the code.

## Working rules

- Chat with the user in Ukrainian. Commits, docs, Jira and code comments are in English.
- Every change ends with a Windows build of the Unreal client in `unreal/CubeWorld/Saved/Packaged/Windows`, so the user can try it. If the change touches the Unreal servers, fix and deploy the servers first.
- Game servers and clients speak wss, never plain ws. The Unreal servers replicate over Iris.
- Server keys never go into git or into an image. `Config/DedicatedServerGame.ini` is gitignored.
