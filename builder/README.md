# House builder

A crew of bots builds a ten-storey house over a whole region of the dev world: 24 × 24, ten storeys of rooms
with furniture, two staircases, a roof with a railing. About 12 000 blocks. The bots are guests of the platform
like any browser player, and they play by the browser client's rules:

- they walk with the client's own physics (`web/physics.js`): no flying, a jump of one block, a fall of up to six;
- they place and break a block only where the client's own ray (`raycastBlocks`, 4.5 blocks from the eyes)
  hits it, and the server checks reach, inventory and the player's hitbox as for anyone;
- a bot out of a block signs in again as a new guest, who starts with 64 of everything, where it stands.

## Run it

```bash
node builder/check.mjs --room pink                                         # the plan, checked in seconds
node --experimental-websocket builder/build.mjs --room pink --dry-run      # a rehearsal, nothing leaves the process
node --experimental-websocket builder/build.mjs --room pink                # the real thing, on dev
```

Options: `--bots 15` (a room holds 16; one place is left free), `--scale 0.25` (dry-run speed), `--stay 60`
(seconds the crew stays on the roof). The region must have a C# room up (`pink`, `purple`, `yellow`, …): the
crew reads `web/config.js` for the API and the client key. Exit code 0 when the inspection finds the house whole.

It takes about ten to twelve minutes: clearing the oaks, building, a repair pass, the inspection. The log says
every 30 s how far the crew is and where the time goes (searching, walking, scaffolding, placing, waiting).

## How it works

| File | What |
|---|---|
| `plan.mjs` | The house: every block with its storey, zone and phase. |
| `crew.mjs` | The crew and the foreman's rules; the repair crew. |
| `bot.mjs` | One builder: the platform, the socket protocol (as `web/app.js`), walking, aiming, scaffolding. |
| `world.mjs` | The world as the bots know it, the client's ray, aiming at a face. |
| `path.mjs` | Where a builder can walk: walk, jump one up, drop up to six. |
| `fake.mjs` | A stand-in server for the dry run, with the real server's rules (`CubeWorldServer.Players.cs`, `World.cs`). |
| `check.mjs` | Puts every block in the crew's order and checks each could be placed at its turn. |
| `build.mjs` | The stages: clear, build, repair, inspect, up to the roof. |

**Zones.** The house is cut into the eight rooms and the two halves of the corridor. A room's builder works from
inside its room and never walks through a door: it builds the room's walls, its furniture and the slab over it, then
climbs out through a hatch over the room's table (two dirt under its feet, out onto the slab, the dirt taken away
from above, the hatch shut) and is in the same room a storey up. Builders beyond ten go to the heaviest rooms and
use the stairs.

**Order.** A zone takes a storey up in phases: the corners and wherever walls meet first (a block between finished
walls on three sides can only be seen from outside), then the walls and the stairs, then the furniture (a cabinet
against a wall would hide the wall), then the slab from scaffolding two high (corners, then the edge, then the rest
from the far side in), then the hatch. Within a phase there is no order by height: a block only goes against one
already there, so a builder finishes everything it can reach from where it stands before it walks on.

**The foreman** is a set of rules, not a bot. A block nobody can reach gets scaffolding: a pillar of dirt the
builder puts up under itself, works from, and takes down again. A builder never stands where the plan still has
to go (the slab would close over it), and one that is walled in all the same breaks out. A step of a zone that no
builder of the zone can reach for 6 s is set aside, 20 s for a block others stand on (a corner, a junction, a
slab's corner); set-aside blocks are patched first when a builder has nothing else, from scaffolding if need be.
A builder that has done nothing for 40 s drops what it is on.

**Repair and inspection.** What is still not right at the end is fixed one block at a time: straight in, from
scaffolding, or, when a neighbour hides it, by taking the neighbour out, filling the hole through the gap and
putting the neighbour back. Then every block of the plan is checked against the server's world.

## The dry run

`--dry-run` replaces `fetch` and `WebSocket` with `fake.mjs`: an in-process server that keeps the C# server's
rules (reach 4.5 + 1 to the block a placement goes against, only into air, never into a player, only in its own
region, starting stacks of 64, break times by hardness, 40 ms of latency each way). The clock is scaled, so a
twelve-minute build is rehearsed in a few minutes. CPU time counts `1 / scale` times over in a dry run, so its
timings are on the slow side.

## The logo

`logo/` paints the PlayServ logo on a wall 64 high on the east edge of the world: `logo/README.md`.
