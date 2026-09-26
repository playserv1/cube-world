# PlayServ logo painter

Eight builders paint the PlayServ logo on a wall 24 wide and 64 high (all the height the world has) on the east
edge of the world, facing west over the whole map: the mark from `playserv-dashboard/public/logo-small.svg` in
gold on stone, "PLAY" on brick and "SERV" on wood in sand letters. There is no white or violet block.

The wall stands in the column x = 71 of the chosen region and reads along +y: looking east in the client, the
camera's right hand is +y (right = forward × up). A wall read along +x from the south comes out mirrored.

The builders play by the browser client's rules, as the house builder's do: the client's physics, the client's
ray (4.5 blocks) for every block placed or broken, dirt pillars for scaffolding.

## Run it

```bash
ROOM=green node --experimental-websocket builder/logo/paint.mjs --probe   # the plan as the west sees it, nothing built
ROOM=green STAY=60 node --experimental-websocket builder/logo/paint.mjs   # the real thing, on dev
```

`ROOM` is any region with a C# room up (`red`, `blue`, `green`, `yellow`, `purple`, `pink`); the east edge is the
world's edge only for `green` and `pink`. The script reads `web/config.js` for the API and the client key.

| Variable | What |
|---|---|
| `STRIPS=2,3` | Paint only these strips (one builder each); a repair after a bomb or a player broke part of it. |
| `RESTORE=66:15:0:dirt` | Put back single blocks (`x:y:z:kind`, comma-separated) and stop. |
| `STAY` | Seconds the crew stays to look at it (default 60). |

## How it works

- **Strips.** Builder k paints the columns 3k..3k+2 from the bottom up, from a dirt pillar in front of its strip that
  it raises a block per row; every block goes on the top face of the one below. About 3.5 minutes for the whole wall.
- **The foreman** reports every 30 s, notices a builder that has placed nothing for 45 s, sets aside a block that
  will not go in, and sends a builder that was killed (a bomb) back to a new pillar.
- **Clean-up.** Coming down, a builder takes its pillar away and any old scaffolding within reach. Before the strips,
  craters in front of the wall are filled; at the end, scaffolding still standing comes down from scaffolding of its own.
- Dirt in the five columns in front of the wall counts as scaffolding: a player's dirt there gets taken down
  (put it back with `RESTORE`).
