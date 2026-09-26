// Two guards for the room under the red and yellow regions.
//
//   node --experimental-websocket tools/guards/guards.mjs snapshot   records the room as it stands now as the blueprint
//   node --experimental-websocket tools/guards/guards.mjs            puts the guards on their posts until Ctrl+C
//
// Local and personal: not part of the game, not committed. The guards are anonymous players of `dev`, signed in with
// the browser client's settings (web/config.js), so they live by the game's rules: reach, digging time, fists.

import { World } from "./world.mjs";
import { Bot } from "./bot.mjs";
import { Guard } from "./guard.mjs";
import { Room } from "./room.mjs";
import { loadClientConfig, loadRoom, saveRoom } from "./config.mjs";

const REGION_COLORS = ["red", "blue", "green", "yellow", "purple", "pink"];
const log = message => console.log(`${new Date().toISOString().slice(11, 19)} ${message}`);

async function join(bot, at) {
  await bot.signIn();
  const rooms = await bot.browse();
  const color = REGION_COLORS[bot.world.regionOf(at.x, at.y)];
  const room = rooms.find(r => r.room_name.startsWith(`${color}-`)) ?? rooms[0];
  if (!room) throw new Error("no room is up");
  await bot.enter(room.room_name, true);
}

async function snapshot() {
  const spec = loadRoom();
  const world = new World();
  const bot = new Bot({ cfg: loadClientConfig(), name: "Guard", world, log });
  await join(bot, spec.entrance);
  const room = Room.snapshot(spec, world);
  saveRoom(room.toJSON());
  for (const [z, rows] of Object.entries(room.toJSON().blueprint).sort((a, b) => b[0] - a[0])) log(`z=${z}\n${rows.join("\n")}`);
  log(`blueprint saved: x ${spec.box.x0}..${spec.box.x1}, y ${spec.box.y0}..${spec.box.y1}, z ${spec.box.z0}..${spec.box.z1}`);
  process.exit(0);
}

async function run() {
  const spec = loadRoom();
  if (!spec.blueprint) throw new Error("room.json has no blueprint yet: run `snapshot` first");
  const room = new Room(spec);
  const cfg = loadClientConfig();
  const world = new World();
  const shared = { guards: new Set(), vandals: new Map() };
  let saveTimer = null;
  const onBlueprint = () => { clearTimeout(saveTimer); saveTimer = setTimeout(() => { saveRoom(room.toJSON()); log("blueprint updated from the owner's building"); }, 2000); };

  const guards = [];
  for (const [role, name] of [["surface", spec.names.surface], ["vault", spec.names.vault]]) {
    const bot = new Bot({ cfg, name, world, log });
    await join(bot, spec.posts[role]);
    shared.guards.add(bot.id);
    guards.push(new Guard({ bot, role, room, shared, log, onBlueprint }));
  }
  log(`on guard: ${guards.map(g => `${g.bot.name} (${g.role})`).join(", ")}`);
  let ticks = 0;
  setInterval(() => {
    if (process.env.GUARDS_DEBUG && ++ticks % 20 === 0)
      for (const g of guards) {
        const p = g.bot.pos, v = [...shared.vandals.keys()].map(id => g.bot.players.get(id)).filter(Boolean)[0];
        log(`· ${g.bot.name} ${g.mode} at ${p.x.toFixed(1)},${p.y.toFixed(1)},${p.z.toFixed(1)} on ${g.bot.room} hp ${g.bot.health} attacks ${g.attacks ?? 0}` + (g.mode === "repair" && g.target ? ` | ${g.target.left} left, at ${g.target.x},${g.target.y},${g.target.z} ${g.target.now}→${g.target.want} nav ${g.nav ? (g.nav.path ? g.nav.path.length : 'none') : '-'}` : "") +
          (v ? ` | ${v.name} at ${v.x.toFixed(1)},${v.y.toFixed(1)},${v.z.toFixed(1)} hp ${v.health} reach ${g.reachToPlayer(v).toFixed(2)}` : ""));
      }
    for (const g of guards) {
      try { g.tick(); g.bot.tick(); } catch (e) { log(`${g.bot.name}: ${e.stack}`); }
    }
  }, 50);
  process.on("SIGINT", () => { for (const g of guards) g.bot.socket?.close(); saveRoom(room.toJSON()); process.exit(0); });
}

(process.argv[2] === "snapshot" ? snapshot() : run()).catch(e => { log(e.stack ?? e.message); process.exit(1); });
