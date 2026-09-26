// Builds the ten-storey house over a whole region with a crew of bots that play by the browser client's rules.
//
//   node --experimental-websocket builder/build.mjs --room pink                  # the real thing, on dev
//   node --experimental-websocket builder/build.mjs --room pink --dry-run        # rehearsal against builder/fake.mjs
//   options: --bots 15 (at most 15: a room holds 16)  --scale 0.1 (dry run speed)  --stay 60 (seconds on the roof)
//
// Stages: clear the region (trees and anything the plan does not have), build the plan storey by storey, repair
// whatever is still not right, inspect every block, and climb to the roof.

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { World, cellOf, key } from "./world.mjs";
import { Bot } from "./bot.mjs";
import { housePlan, STOREYS, H, ROOF } from "./plan.mjs";
import { Site, work, repair } from "./crew.mjs";
import { reach, pathTo } from "./path.mjs";
import { FakeServer } from "./fake.mjs";
import { setScale, sleep, now, log } from "./clock.mjs";

const args = process.argv.slice(2);
const opt = (name, fallback) => { const i = args.indexOf(`--${name}`); return i < 0 ? fallback : args[i + 1]; };
const flag = name => args.includes(`--${name}`);
const COLORS = ["red", "blue", "green", "yellow", "purple", "pink"];
const color = opt("room", "pink");
const region = COLORS.indexOf(color);
if (region < 0) throw new Error(`--room must be one of ${COLORS.join(", ")}`);
const COUNT = Math.min(15, Number(opt("bots", 15)));
const DRY = flag("dry-run");
if (DRY) setScale(Number(opt("scale", 0.1)));
const STAY = Number(opt("stay", 60));
const NAMES = ["Bob", "Wendy", "Scoop", "Muck", "Dizzy", "Roley", "Lofty", "Pilchard", "Spud", "Travis", "Benny", "Rex", "Molly", "Jack", "Sumsy"];

// The browser client's settings (web/config.js): the API, the public client key, the room type.
function clientConfig() {
  const window = {};
  new Function("window", readFileSync(fileURLToPath(new URL("../web/config.js", import.meta.url)), "utf8"))(window);
  if (!window.CUBEWORLD?.clientKey) throw new Error("web/config.js has no CUBEWORLD.clientKey (copy web/config.example.js)");
  return window.CUBEWORLD;
}

const rx = (region % 3) * 24, ry = Math.floor(region / 3) * 24;
const inside = (x, y) => x >= rx && x < rx + 24 && y >= ry && y < ry + 24;
const fake = DRY ? new FakeServer({ color }) : null;
const cfg = DRY ? { api: "https://dry.run", clientKey: "pk_dry", slug: "cubeworld" } : clientConfig();
const net = fake ? { fetch: fake.fetch, WebSocket: fake.WebSocket } : undefined;
const world = new World();
const quietLog = () => {};

const started = now();
const bots = NAMES.slice(0, COUNT).map(name => new Bot({ cfg, name, world, inside, log: quietLog, net }));
await bots[0].signIn();
const rooms = await bots[0].browse();
const room = rooms.find(r => r.room_name.startsWith(`${color}-`));
if (!room) throw new Error(`no ${color} room is up`);
if (room.capacity - room.players < COUNT) log(`${room.room_name} has ${room.capacity - room.players} places free for ${COUNT} builders: the rest wait their turn`);
// All in at once, a little apart (the platform takes a burst of sign-ins and joins without trouble).
await Promise.all(bots.map(async (bot, i) => {
  await sleep(i * 100);
  if (!bot.player) await bot.signIn();
  bot.roomSlugs = { ...bots[0].roomSlugs };
  for (let attempt = 1; ; attempt++) {
    try { await bot.enter(room.room_name, true); break; }
    catch (e) { if (attempt >= 10) throw e; await sleep(1000 * attempt); }
  }
  bot.start();
}));
log(`${bots.length} builders on ${room.room_name}${DRY ? " (dry run)" : ""}`);

const plan = housePlan(rx, ry);
const planKeys = new Set(plan.map(cellOf));
const site = new Site({ world, inside, bots, everyone: bots, plan });
site.x0 = rx; site.y0 = ry;
const tally = list => Object.entries(list.reduce((m, b) => (m[b.kind] = (m[b.kind] ?? 0) + 1, m), {})).sort((a, b) => b[1] - a[1]).map(e => e.join(" ")).join(", ");

// 1. Clear: the oaks and anything else above ground that the plan does not have, and any wrong block in a plan cell.
const want = new Map(plan.map(b => [cellOf(b), b]));
const clear = [];
for (let x = rx; x < rx + 24; x++) for (let y = ry; y < ry + 24; y++) for (let z = 0; z <= ROOF + 2; z++) {
  const kind = world.onServer(x, y, z);
  if (kind === "air") continue;
  const w = want.get(key(x, y, z));
  if (!w || w.kind !== kind) clear.push({ x, y, z, kind });
}
log(`clearing ${clear.length} blocks: ${tally(clear)}`);
let left = await work(site, clear, "dig", { label: "clearing" });
log(left.length ? `clearing: ${left.length} left standing` : "the site is clear");

// 2. Build.
const jobs = plan.filter(b => world.onServer(b.x, b.y, b.z) !== b.kind);
log(`building ${jobs.length} blocks: ${tally(jobs)}`);
const buildStart = now();
left = await work(site, jobs, "place", { label: "building" });
log(`built in ${((now() - buildStart) / 60000).toFixed(1)} min, ${left.length} not yet right`);

// Anything in the region the plan does not have: a leaf the clearing could not reach, a stray block.
const strays = () => {
  const out = [];
  for (let x = rx; x < rx + 24; x++) for (let y = ry; y < ry + 24; y++) for (let z = 0; z <= ROOF + 2; z++) {
    const kind = world.onServer(x, y, z), w = want.get(key(x, y, z));
    if (kind !== "air" && !w && !site.scaffold.has(key(x, y, z))) out.push({ x, y, z, kind });
  }
  return out;
};
{
  const extra = strays();
  if (extra.length) { log(`breaking out ${extra.length} blocks the plan does not have: ${extra.slice(0, 6).map(b => `${cellOf(b)} ${b.kind}`).join(", ")}`); await work(site, extra, "dig", { label: "strays", quiet: true }); }
}

// 3. Repair what is still not right; take down any scaffolding left standing (it may sit in a plan cell), and repair again.
const notRight = () => plan.filter(b => world.onServer(b.x, b.y, b.z) !== b.kind);
for (let round = 1; round <= 2 && (notRight().length || site.scaffold.size); round++) {
  if (notRight().length) {
    const stats = await repair(site, plan);
    log(`repair: ${stats.straight} straight in, ${stats.through} through a gap, ${stats.broken} wrong blocks broken out`);
  }
  const leftover = [...site.scaffold].map(k => { const [x, y, z] = k.split(":").map(Number); return { x, y, z, kind: "dirt" }; })
    .filter(b => world.onServer(b.x, b.y, b.z) === "dirt" && !(want.get(cellOf(b))?.kind === "dirt"));
  if (leftover.length) {
    log(`taking down ${leftover.length} blocks of scaffolding left standing`);
    await work(site, leftover, "dig", { label: "scaffolding", quiet: true });
    for (const b of leftover) if (world.onServer(b.x, b.y, b.z) === "air") site.scaffold.delete(cellOf(b));
  }
}

// 4. Inspect every block of the plan.
const wrong = b => world.onServer(b.x, b.y, b.z) !== b.kind;
let missing = 0;
for (let s = -1; s < STOREYS; s++) {
  const all = plan.filter(b => b.storey === s), bad = all.filter(wrong);
  missing += bad.length;
  log(`inspection, ${s < 0 ? "ground" : `storey ${s + 1}`}: ${all.length - bad.length}/${all.length}${bad.length ? `, missing ${bad.slice(0, 20).map(b => `${cellOf(b)} ${b.kind} (${b.role})`).join(", ")}${bad.length > 20 ? " …" : ""}` : ""}`);
}
log(`scaffolding left: ${site.scaffold.size}`);
const extraLeft = strays();
missing += extraLeft.length;
log(`blocks the plan does not have: ${extraLeft.length}${extraLeft.length ? ` (${extraLeft.slice(0, 10).map(b => `${cellOf(b)} ${b.kind}`).join(", ")})` : ""}`);
log(`${missing ? `${missing} blocks missing` : "the house is complete"}; ${((now() - started) / 60000).toFixed(1)} min from the first builder in`);
for (const b of bots) log(`  ${b.name}: placed ${b.stats.placed}, dug ${b.stats.dug}, scaffolding ${b.stats.scaffold}, fetched blocks ${b.stats.relogs} times`);
if (fake) log(`server: ${fake.counts.placed} placed, ${fake.counts.refused} refused, ${fake.counts.broken} broken`);

// 5. Everyone up to the roof.
await Promise.all(bots.map(async (b, i) => {
  await sleep(i * 500);
  await b.settle();
  const r = reach(world, inside, b.cell());
  const spots = r.cells.filter(c => c.z === ROOF + 1);
  if (spots.length) await b.go(pathTo(r, spots[(i * 37) % spots.length]));
}));
await sleep(STAY * 1000);
for (const b of bots) b.stop();
fake?.stop();
process.exit(missing ? 1 : 0);
