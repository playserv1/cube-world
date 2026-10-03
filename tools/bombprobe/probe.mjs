// A bot walks across a region border straight through a free bomb that lies close to it, and says tick by tick where
// it was, which room it was in, whether a crossing was under way, and when (if ever) the bomb came into its hand.
//
//   node --experimental-websocket tools/bombprobe/probe.mjs [--trials 6] [--near 3] [--from-bomb-side]
//
// Each trial is a new guest (one that ends holding a bomb leaves with it; the drop function fizzles it a minute on).

import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { World } from "../../builder/world.mjs";
import { Bot } from "../../builder/bot.mjs";
import { reach, pathTo } from "../../builder/path.mjs";
import { sleep } from "../../builder/clock.mjs";

const args = process.argv.slice(2);
const opt = (name, fallback) => { const i = args.indexOf(`--${name}`); return i < 0 ? fallback : args[i + 1]; };
const TRIALS = Number(opt("trials", 6)), NEAR = Number(opt("near", 3));
const REACH = 0.3 + 1.0;           // half the player's width + PickupReach (Spec.cs)

function clientConfig() {
  const window = {};
  new Function("window", readFileSync(fileURLToPath(new URL("../../web/config.js", import.meta.url)), "utf8"))(window);
  return window.CUBEWORLD;
}
const cfg = clientConfig();
const t0 = Date.now();
const log = (...a) => console.log(((Date.now() - t0) / 1000).toFixed(2).padStart(7), ...a);

class Probe extends Bot {
  constructor(o) { super(o); this.bombs = new Map(); this.trace = null; }
  onFrame(frame, teleport) {
    if (frame.type === "welcome") {
      this.bombs.clear();
      for (const f of frame.bombs ?? []) this.bomb(f, "welcome");
      this.trace?.push({ t: Date.now(), ev: `welcome from ${this.room}` });
    }
    if (frame.type === "bomb") this.bomb(frame, "frame");
    super.onFrame(frame, teleport);
  }
  bomb(f, how) {
    const b = { ...f.bomb, height: f.z, heardAt: Date.now() };
    const old = this.bombs.get(b.bomb_id);
    this.bombs.set(b.bomb_id, b);
    if (this.watch === b.bomb_id && old?.state !== b.state)
      this.trace?.push({ t: Date.now(), ev: `bomb ${b.state}${b.holder ? ` holder=${b.holder === this.id ? "ME" : b.holder}` : ""} (${how}, room ${this.room})` });
  }
}

const RS = 24, COLS = 3, ROWS = 2;
const regionOf = (x, y) => Math.floor(y / RS) * COLS + Math.floor(x / RS);

/** A free bomb at rest within NEAR of a border to another live region, with the side to come from. */
function candidates(bot) {
  const live = new Set(bot.regions.map(r => Number(r.region)));
  const out = [];
  for (const b of bot.bombs.values()) {
    if (b.state !== "free" || Date.now() - b.heardAt > 120000) continue;
    const r = regionOf(b.x, b.y);
    if (!live.has(r)) continue;
    const x0 = (r % COLS) * RS, y0 = Math.floor(r / COLS) * RS;
    const sides = [
      { d: b.x - x0, axis: "x", dir: -1, n: b.x - x0 >= 0 && r % COLS > 0 ? r - 1 : -1 },
      { d: x0 + RS - b.x, axis: "x", dir: 1, n: r % COLS < COLS - 1 ? r + 1 : -1 },
      { d: b.y - y0, axis: "y", dir: -1, n: Math.floor(r / COLS) > 0 ? r - COLS : -1 },
      { d: y0 + RS - b.y, axis: "y", dir: 1, n: Math.floor(r / COLS) < ROWS - 1 ? r + COLS : -1 },
    ];
    for (const s of sides) if (s.n >= 0 && live.has(s.n) && s.d <= NEAR) out.push({ bomb: b, region: r, ...s });
  }
  return out.sort((a, b) => a.d - b.d);
}

/** The bomb in the bot's hand, as its room last said. */
const inHand = bot => [...bot.bombs.values()].find(x => x.state === "held" && x.holder === bot.id);

function roomLabel(bot, region) {
  const r = bot.regions.find(r => Number(r.region) === region);
  return r ? `${r.room} (${bot.roomSlugs[r.room] ?? r.slug ?? "?"})` : `region ${region}`;
}

async function trial(n, used) {
  const world = new World();
  const bot = new Probe({ cfg, name: `Probe${n}`, world, inside: () => true, log });
  bot.inside = (x, y) => x >= 0 && y >= 0 && x < world.width && y < world.depth;
  await bot.signIn();
  const rooms = await bot.browse();
  await bot.enter(rooms[0].room_name, true);
  bot.start();
  await bot.settle();

  // Wait for a bomb at rest close to a border.
  let c;
  for (let i = 0; i < 180; i++) {
    c = candidates(bot).find(c => !used.has(c.bomb.bomb_id) && c.bomb.height <= 6 && Date.now() - c.bomb.at > 18000);
    if (c) break;
    await sleep(1000);
  }
  if (!c) { log(`trial ${n}: no bomb near a border in 3 min`); bot.stop(); return null; }
  used.add(c.bomb.bomb_id);
  const b = c.bomb;
  const fromBombSide = args.includes("from-bomb-side");
  // The line through the bomb across the border; start 5 cells into the neighbour, end 4 past the bomb.
  const along = c.axis === "x" ? Math.floor(b.y) : Math.floor(b.x);
  const borderAt = c.axis === "x" ? (c.dir < 0 ? (c.region % COLS) * RS : (c.region % COLS + 1) * RS)
                                  : (c.dir < 0 ? Math.floor(c.region / COLS) * RS : (Math.floor(c.region / COLS) + 1) * RS);
  const startAcross = c.dir < 0 ? borderAt - 5 : borderAt + 4;
  const endAcross = c.dir < 0 ? Math.floor(c.axis === "x" ? b.x : b.y) + 4 : Math.floor(c.axis === "x" ? b.x : b.y) - 5;
  const cellAt = (across) => c.axis === "x" ? { x: across, y: along } : { x: along, y: across };
  log(`trial ${n}: bomb ${b.bomb_id} at ${b.x},${b.y} h=${b.height?.toFixed(2)} in ${roomLabel(bot, c.region)}, ${c.d.toFixed(2)} from the border with ${roomLabel(bot, c.n)}`);

  const goTo = async cell => {
    for (let k = 0; k < 4; k++) {
      const here = bot.cell(), r = reach(world, bot.inside, here);
      const target = r.cells.filter(q => q.x === cell.x && q.y === cell.y).sort((a, b) => a.z - b.z)[0];
      if (!target) return false;
      if (await bot.go(pathTo(r, target))) return true;
      await sleep(500);
    }
    return false;
  };
  const [first, last] = fromBombSide ? [cellAt(endAcross), cellAt(startAcross)] : [cellAt(startAcross), cellAt(endAcross)];
  if (!await goTo(first)) { log(`trial ${n}: cannot get to the start ${first.x},${first.y}`); bot.stop(); return null; }
  for (let i = 0; i < 100 && (bot.switching || roomLabel(bot, regionOf(bot.body.x, bot.body.z)).split(" ")[0] !== bot.room); i++) await sleep(100);
  await sleep(1000);
  if (bot.bombs.get(b.bomb_id)?.state !== "free") { log(`trial ${n}: the bomb is ${bot.bombs.get(b.bomb_id)?.state} before the walk, skipped`); bot.stop(); return null; }
  // A player holds one bomb at a time (both servers): one picked up on the way here leaves the watched one on the ground.
  const before = inHand(bot);
  if (before) { log(`trial ${n}: the bot already holds ${before.bomb_id} (picked up on the way), skipped`); bot.stop(); return null; }

  bot.watch = b.bomb_id;
  bot.trace = [{ t: Date.now(), ev: `start in ${bot.room}` }];
  const ticks = [];
  const sampler = setInterval(() => {
    const p = bot.body, dx = Math.abs(b.x - p.x), dy = Math.abs(b.y - p.z);
    ticks.push({ t: Date.now(), x: p.x, y: p.z, z: p.y, region: regionOf(p.x, p.z), room: bot.room, switching: bot.switching, inReach: dx <= REACH && dy <= REACH });
  }, 50);
  const walked = await goTo(last);
  await sleep(3000);
  clearInterval(sampler);

  const mine = bot.bombs.get(b.bomb_id);
  const got = mine?.state === "held" && mine.holder === bot.id;
  // Ticks in reach, with whose room the player was in and which region they stood in.
  const reachTicks = ticks.filter(t => t.inReach);
  if (!got) {
    // What else stops a pickup: another bomb in the hand, or a bomb resting out of reach up or down (the servers take one
    // from 0.5 below the feet to 0.5 above the head, Spec.PickupReachUp), e.g. on an oak's leaves over the bot's head.
    const other = inHand(bot), feet = reachTicks.map(t => t.z);
    log(`trial ${n}: not in hand; ${other ? `the bot holds ${other.bomb_id}` : "the hand is empty"}; the bomb rests at ${mine?.height?.toFixed(2)}`
      + (feet.length ? `, the feet in reach at ${Math.min(...feet).toFixed(2)}–${Math.max(...feet).toFixed(2)}` : ""));
  }
  const byHost = {};
  for (const t of reachTicks) {
    const k = `${t.switching ? "switching, " : ""}host=${t.room} stands in ${roomLabel(bot, t.region).split(" ")[0]}`;
    byHost[k] = (byHost[k] ?? 0) + 1;
  }
  log(`trial ${n} (${bot.id}): ${walked ? "walked" : "walk cut short"}; bomb ${mine?.state}${got ? " IN HAND" : ""}; ${reachTicks.length} ticks in reach:`, byHost);
  const events = [...bot.trace, ...ticks.filter((t, i) => i === 0 || t.room !== ticks[i - 1].room || t.switching !== ticks[i - 1].switching || t.inReach !== ticks[i - 1].inReach || t.region !== ticks[i - 1].region)
    .map(t => ({ t: t.t, ev: `pos ${t.x.toFixed(2)},${t.y.toFixed(2)} region=${t.region} host=${t.room}${t.switching ? " SWITCHING" : ""}${t.inReach ? " IN-REACH" : ""}` }))].sort((a, b) => a.t - b.t);
  for (const e of events) console.log(`          +${((e.t - bot.trace[0].t) / 1000).toFixed(2)}s ${e.ev}`);
  bot.stop();
  return { bomb: b.bomb_id, d: c.d, got, reachTicks: reachTicks.length, byHost, bombRoom: roomLabel(bot, c.region), fromRoom: roomLabel(bot, c.n) };
}

const used = new Set(), results = [];
for (let n = 1; n <= TRIALS; n++) {
  try { const r = await trial(n, used); if (r) results.push(r); }
  catch (e) { log(`trial ${n}: ${e.message}`); }
}
console.log("\nSUMMARY");
for (const r of results) console.log(`${r.got ? "picked  " : "MISSED  "} d=${r.d.toFixed(2)} ${r.fromRoom} → ${r.bombRoom}  reach ticks ${r.reachTicks}`, JSON.stringify(r.byHost));
process.exit(0);
