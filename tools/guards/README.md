# Room guards (local, not committed)

Two bots, *Roof Guard* and *Vault Guard*, keep the underground room at x 0..12, y 20..42 (red and yellow
regions, entrance hole at 5..6, 40..41 in yellow). They are anonymous players of `dev`, signed in with
`web/config.js`, and obey the game's rules (reach, dig time, fists, inventory).

```bash
node --experimental-websocket tools/guards/guards.mjs snapshot   # record the room as it stands now
node --experimental-websocket tools/guards/guards.mjs            # guard it until Ctrl+C
GUARDS_DEBUG=1 node --experimental-websocket tools/guards/guards.mjs   # with a status line per guard each second
GUARDS_ROOM=/path/other.json node --experimental-websocket tools/guards/guards.mjs   # another room file
```

- **Roof Guard** (post 3.5, 40.5) restores the top layer (z = -1): the gold roof, the entrance frame, the grass.
- **Vault Guard** (post 8.5, 40.5) restores everything below (z -3..-2), follows a visitor inside 3-5 blocks
  behind and goes back to its post when they leave. Whoever breaks or places anything down there, or anything at
  all while inside, is beaten until dead (followed across servers, let go beyond 30 blocks from the entrance).
- Both look at whoever comes within 8 blocks of the entrance; otherwise each looks the way its post's `yaw` says
  (π: north, towards the room).
- A room that is not on the map (a new world, a reset) is built from nothing the same way: each guard digs out
  and fills in its part, block by block, and the log says when a part is being built and when it is whole.
- A guard out of a block it needs signs in again as a new anonymous player, who starts with 64 of everything
  (both servers keep a player's inventory between visits, so the same player would come back as empty). It keeps
  its place; at most once every 10 s.
- Crossing a border is seamless as for the clients: moves carry the server's correction number (`seq`), and a
  correction is taken; a crossing in progress is never started twice.
- `owner` in `room.json` (Artem) is ignored: not watched, followed or fought. A guard within 4 blocks bows its
  head. What the owner builds or breaks in the box becomes the blueprint (`room.json` is rewritten).
- `room.json` holds the box, posts, the walking area and the blueprint (one picture per level, row per y,
  character per x; legend in `room.mjs`). Edit it by hand, or rebuild and run `snapshot`.
