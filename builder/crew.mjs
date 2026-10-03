// The crew at work, and the foreman who keeps it going.
//
// Each builder owns a strip of columns (cut so that every strip holds about as many blocks) and takes its strip up
// storey by storey, phase by phase (plan.mjs rank): it walks to a place it can reach the next blocks from, and
// places one after another as a held button does. The foreman's part is rules, not a person:
//   - a block nobody can reach from where they can stand gets scaffolding: a pillar of dirt the builder puts up
//     under itself, works from, and takes down again;
//   - a builder out of a block it needs signs in again as a new guest, with full stacks, where it stands;
//   - a builder with nothing to do in its own strip lends a hand with whatever is near in the next one;
//   - a block that keeps failing is set aside, so the strip goes on; set-aside blocks are patched later;
//   - a builder that has done nothing for 40 s is told to drop what it is on;
//   - every 30 s, a report.
// At the end the inspector goes over every block of the plan, and the repair crew fills what is still missing,
// taking a neighbour out where one hides the hole and putting it back.

import { key, cellOf, DIRS, aimPlace, aimDig, eyeOfCell, overlaps } from "./world.mjs";
import { reach, pathTo, standable } from "./path.mjs";
import { rank, H, tableOf, ROOM_ZONES } from "./plan.mjs";
import { now, sleep, every, log } from "./clock.mjs";

const bodyIn = (s, b) => s.x === b.x && s.y === b.y && (b.z === s.z || b.z === s.z + 1);
// Dirt a builder keeps back for scaffolding: the slabs of two storeys are dirt too.
const DIRT_RESERVE = 8;
const STRUCTURAL = new Set(["corner", "junction", "slab corner"]);

export class Site {
  constructor({ world, inside, bots, everyone, plan }) {
    Object.assign(this, { world, inside, bots, everyone, plan });
    this.planKeys = new Set(plan.map(cellOf));
    this.scaffold = new Set();
    this.claimed = new Map();         // block key -> bot, while a bot is on it
    this.standing = new Map();        // bot -> the cell it is heading for
    this.reachCache = new Map();      // bot -> { at, from, r }
  }

  someoneIn(b) { return this.everyone.some(o => o.body && o.placed && overlaps(o.body, b)); }

  reachOf(bot) {
    const c = bot.cell(), k = cellOf(c), cached = this.reachCache.get(bot);
    if (cached && cached.from === k && now() - cached.at < 800) return cached.r;
    const r = reach(this.world, this.inside, c);
    this.reachCache.set(bot, { at: now(), from: k, r });
    return r;
  }

  /** Whether a cell is inside a room zone (a room's builder works from inside its room). */
  inRoom(zone, c) {
    const r = ROOM_ZONES.find(z => z.name === zone);
    return !r || (c.x >= this.x0 + r.i0 && c.x <= this.x0 + r.i1 && c.y >= this.y0 + r.j0 && c.y <= this.y0 + r.j1);
  }

  /**
   * Where to go next: for each of the first few candidates its nearest stand, scored by how many of the band could
   * be placed from there (a stand by one block only, with the rest out of sight, costs a walk for each).
   */
  bestStand(bot, cands, how, room) {
    let best = null;
    for (const b of cands.slice(0, 6)) {
      const s = this.stand(bot, b, how, room) ?? this.stand(bot, b, how);
      if (!s) continue;
      const eye = eyeOfCell(s.stand);
      let n = 0;
      for (const o of cands) {
        if (Math.abs(o.x - s.stand.x) > 5 || Math.abs(o.y - s.stand.y) > 5 || Math.abs(o.z - s.stand.z - 1) > 5) continue;
        if (how === "place" ? aimPlace(this.world, eye, o) : aimDig(this.world, eye, o)) n++;
        if (n >= 30) break;
      }
      const score = n - s.path.length * 0.3;
      if (!best || score > best.score) best = { ...s, b, score };
    }
    return best;
  }

  /** Where the bot can walk to and do b from: the nearest such cell (inside its room, if one is given), and the way. */
  stand(bot, b, how, room) {
    const here = bot.cell();
    if (!standable(this.world, this.inside, here.x, here.y, here.z)) return null;
    const r = this.reachOf(bot);
    const others = new Set(this.bots.filter(o => o !== bot).flatMap(o => [cellOf(o.cell()), this.standing.get(o)].filter(Boolean)));
    let best = null;
    for (let x = b.x - 5; x <= b.x + 5; x++) for (let y = b.y - 5; y <= b.y + 5; y++) for (let z = b.z - 6; z <= b.z + 3; z++) {
      const k = key(x, y, z), d = r.dist.get(k);
      if (d === undefined || (best && d >= best.d)) continue;
      const s = { x, y, z };
      if (bodyIn(s, b) || others.has(k)) continue;
      if (room && !this.inRoom(room, s)) continue;
      if (this.pendingPlace && (this.pendingPlace.has(k) || this.pendingPlace.has(key(x, y, z + 1)))) continue;
      if (how === "dig" && this.pendingDig?.has(key(x, y, z - 1))) continue;
      const ok = how === "place" ? aimPlace(this.world, eyeOfCell(s), b) : aimDig(this.world, eyeOfCell(s), b);
      if (ok) best = { s, d };
    }
    return best && { stand: best.s, path: pathTo(r, best.s) };
  }

  /** Scaffolding for a block nobody can reach: a column to pillar up, the lowest that reaches it. */
  scaffoldFor(bot, b, how) {
    const here = bot.cell();
    if (!standable(this.world, this.inside, here.x, here.y, here.z)) return null;
    const r = this.reachOf(bot);
    let best = null;
    for (let gx = b.x - 4; gx <= b.x + 4; gx++) for (let gy = b.y - 4; gy <= b.y + 4; gy++) for (let gz = b.z - 8; gz <= b.z; gz++) {
      const g = { x: gx, y: gy, z: gz }, d = r.dist.get(cellOf(g));
      if (d === undefined) continue;
      for (let h = gz + 1; h <= Math.min(gz + 8, b.z + 1); h++) {
        // The pillar and the body on top must be clear and out of the plan; the head may be in a plan cell still empty.
        let clear = true;
        for (let z = gz; z <= h + 1 && clear; z++)
          if (this.world.solid(gx, gy, z) || (z <= h && this.planKeys.has(key(gx, gy, z))) || this.scaffold.has(key(gx, gy, z))) clear = false;
        if (!clear) break;
        const s = { x: gx, y: gy, z: h };
        if (bodyIn(s, b)) continue;
        const cost = d + 4 * (h - gz);
        if (best && cost >= best.cost) break;
        const ok = how === "place" ? aimPlace(this.world, eyeOfCell(s), b) : aimDig(this.world, eyeOfCell(s), b);
        if (ok) { best = { g, h, cost, path: pathTo(r, g) }; break; }
      }
    }
    return best;
  }

  onScaffold(bot) { const c = bot.cell(); return this.scaffold.has(key(c.x, c.y, c.z - 1)); }
}

/** Cuts the region's columns into n strips with about as many blocks each. */
export function strips(jobs, x0, n) {
  const per = new Array(24).fill(0);
  for (const b of jobs) per[b.x - x0]++;
  const total = per.reduce((a, b) => a + b, 0), out = [];
  let start = 0, acc = 0;
  for (let i = 0; i < 24; i++) {
    acc += per[i];
    const left = n - out.length;
    if (left > 1 && (acc >= total * (out.length + 1) / n || 24 - i - 1 < left - 1)) { out.push([x0 + start, x0 + i]); start = i + 1; }
  }
  out.push([x0 + start, x0 + 23]);
  while (out.length < n) out.push(out[out.length - 1]);
  return out;
}

/**
 * Runs a crew over a list of jobs: how is "place" (the plan, in rank order) or "dig" (clearing).
 * Resolves with what is left undone.
 */
export async function work(site, jobs, how, { label, room, quiet = false }) {
  const { world, bots } = site;
  const byKey = new Map(jobs.map(j => [cellOf(j), j]));
  const pending = new Set(byKey.keys());
  const done = b => how === "place" ? world.onServer(b.x, b.y, b.z) === b.kind : world.onServer(b.x, b.y, b.z) === "air";
  const looksDone = b => how === "place" ? world.kindAt(b.x, b.y, b.z) === b.kind : !world.solid(b.x, b.y, b.z);
  const hook = k => { const b = byKey.get(k); if (!b) return; if (done(b)) pending.delete(k); else pending.add(k); };
  world.hooks.add(hook);
  for (const k of [...pending]) hook(k);
  if (how === "dig") site.pendingDig = pending; else site.pendingPlace = pending;
  const anchored = b => how === "dig" || DIRS.some(([dx, dy, dz]) => world.solid(b.x + dx, b.y + dy, b.z + dz));
  // The slab beside one over a wall waits for it when the slab on the wall's other side is already in, whoever's
  // zone it is in: with both its neighbours in, that one would be seen from above only, and the storey above covers it.
  const besideWall = b => b.role === "slab" && [[1, 0], [-1, 0], [0, 1], [0, -1]].some(([dx, dy]) => {
    const k = key(b.x + dx, b.y + dy, b.z);
    return pending.has(k) && byKey.get(k).role === "slab over wall" && world.solid(b.x + 2 * dx, b.y + 2 * dy, b.z);
  });
  const aside = new Set(), tries = new Map(), cool = new Map(), why = {};
  // Where the time goes, for the report: searching for the next job, walking, putting up scaffolding, the bursts.
  const spent = { search: 0, walk: 0, scaffold: 0, burst: 0, bursts: 0, blocks: 0, idle: 0 };
  const because = r => { why[r] = (why[r] ?? 0) + 1; };
  const cooled = k => (cool.get(k) ?? 0) > now();
  const live = k => pending.has(k) && !aside.has(k);
  const total = jobs.length;
  // Each builder owns zones (plan.mjs): a room, or half the corridor. More builders than zones go to the corridor.
  const laneOf = new Map(), climber = new Map();     // zone -> the builder who climbs out through its hatch
  if (how === "place") {
    const zones = [...new Set(jobs.map(j => j.zone))];
    const rooms = zones.filter(z => z.startsWith("room")), halls = zones.filter(z => !z.startsWith("room"));
    bots.forEach((b, i) => laneOf.set(b, new Set()));
    const load = z => jobs.filter(j => j.zone === z).length;
    [...rooms, ...halls].forEach((z, i) => { const b = bots[i % bots.length]; laneOf.get(b).add(z); if (!climber.has(z)) climber.set(z, b); });
    // more builders than zones: the extra ones go to the heaviest rooms. Only the first builder of a room climbs out
    // through its hatch; a second one goes up by the stairs and comes back in through the door.
    const heavy = [...rooms].sort((a, b) => load(b) - load(a));
    for (let i = rooms.length + halls.length; i < bots.length; i++) laneOf.get(bots[i]).add(heavy[(i - rooms.length - halls.length) % heavy.length]);
    if (bots.length < rooms.length + halls.length) log(`[${label}] ${bots.length} builders for ${rooms.length + halls.length} zones: some take two`);
    log(`[${label}] zones: ${bots.map(b => `${b.name} ${[...laneOf.get(b)].join("+")} (${[...laneOf.get(b)].reduce((n, z) => n + load(z), 0)})`).join(", ")}`);
  } else bots.forEach(b => laneOf.set(b, null));
  for (const j of jobs) j.step = Math.floor(rank(j) / 1000);
  // Each zone's own jobs, so a builder looks through its zone, not the whole house.
  const laneKeys = new Map();
  const keysIn = lane => {
    const id = lane ? [...lane].sort().join("|") : "all";
    if (!laneKeys.has(id)) laneKeys.set(id, jobs.filter(b => !lane || lane.has(b.zone)).map(cellOf));
    return laneKeys.get(id);
  };
  const status = new Map();
  let finished = false;

  // The band a builder works in: the lowest step of its own strip (storey and phase) that still has a live block.
  function band(bot) {
    let low = Infinity;
    for (const k of keysIn(laneOf.get(bot))) { if (!pending.has(k) || aside.has(k)) continue; const st = byKey.get(k).step; if (st < low) low = st; }
    return low;
  }
  function candidates(bot, { stock = true, lane = laneOf.get(bot), bandOf = bot } = {}) {
    const low = how === "place" ? band(bandOf) : null, c = bot.cell();
    const out = [];
    for (const k of keysIn(lane)) {
      if (!pending.has(k) || aside.has(k) || site.claimed.has(k) || world.unconfirmed.has(k) || cooled(k)) continue;
      const b = byKey.get(k);
      if (how === "place" && b.step !== low) continue;
      if (b.role === "hatch" && (bandOf !== bot || climber.get(b.zone) !== bot)) continue;   // a hatch is its climber's way up
      if (!anchored(b) || besideWall(b) || (stock && how === "place" && (bot.inventory[b.kind] ?? 0) <= (b.kind === "dirt" ? DIRT_RESERVE : 0))) continue;
      out.push(b);
    }
    // The slab is laid from the far side in: a block already down in between would hide the ones beyond it.
    const far = b => b.phase === 3 && (b.role === "slab" || b.role === "slab over wall");
    return out.sort((a, b) => (rank(a) - rank(b)) + (Math.hypot(a.x - c.x, a.y - c.y) - Math.hypot(b.x - c.x, b.y - c.y)) * (far(a) && far(b) ? -1 : 1));
  }

  // A burst: from where it stands, one block after another, every 4 ticks. What is within reach is gathered once,
  // as the burst starts (with the blocks over them, which become placeable as the ones below go in), and only that
  // short list is looked through for the next block.
  async function burst(bot, first) {
    let n = 0, target = first;
    const c0 = bot.cell(), low = how === "place" ? band(bot) : null;
    const pool = keysIn(laneOf.get(bot)).map(k => byKey.get(k))
      .filter(b => Math.abs(b.x - c0.x) <= 5 && Math.abs(b.y - c0.y) <= 5 && Math.abs(b.z - c0.z - 1) <= 5 && (how !== "place" || b.step === low))
      .sort((a, b) => Math.hypot(a.x - c0.x, a.y - c0.y, a.z - c0.z) - Math.hypot(b.x - c0.x, b.y - c0.y, b.z - c0.z));
    while (target && !finished) {
      const k = cellOf(target);
      site.claimed.set(k, bot);
      const result = how === "place" ? await bot.place(target, { fine: n === 0, everyone: site.everyone }) : await bot.dig(target);
      setTimeout(() => { if (site.claimed.get(k) === bot) site.claimed.delete(k); }, 2500);
      if (result !== "ok") { because(result); fail(k, target); if (process.env.BUILDER_DEBUG === bot.name) log(`    ${bot.name}: ${result} at ${k} from ${JSON.stringify(bot.cell())}`); if (n === 0) return 0; }
      else n++;
      const open = pool.filter(b => live(cellOf(b)) && !site.claimed.has(cellOf(b)) && !looksDone(b) && !cooled(cellOf(b)) && anchored(b)
        && (how !== "place" || (bot.inventory[b.kind] ?? 0) > 0) && (b.role !== "hatch") && !besideWall(b) && !site.someoneIn(b));
      // the slab from the far side in; walls and the rest the nearest first
      if (open.length && open[0].phase === 3) open.reverse();
      target = open.slice(0, 20).find(b => how === "place" ? aimPlace(world, bot.eye, b) : aimDig(world, bot.eye, b)) ?? null;
      if (!target && process.env.BUILDER_DEBUG === bot.name) bot.burstEnd = `${open.length} open in reach, none in aim; pool ${pool.length}`;
    }
    return n;
  }

  function fail(k, b) {
    cool.set(k, now() + 2000);
    if ((tries.set(k, (tries.get(k) ?? 0) + 1).get(k)) >= 6) { aside.add(k); if (!quiet) log(`[${label}] setting ${k} ${b.kind ?? ""} aside`); }
  }

  // Up a storey through the hatch: onto the table, two dirt under the feet, out on to the slab, the dirt away, shut it.
  async function climb(bot, hatch) {
    const t = tableOf(hatch.zone, site.x0, site.y0);
    if (!t) return false;
    const base = hatch.z - 3;
    const r = site.reachOf(bot), onTable = { x: t.x, y: t.y, z: base + 1 };
    if (!r.dist.has(cellOf(onTable))) return false;
    status.set(bot, "climbing out through the hatch");
    if (!(await bot.go(pathTo(r, onTable)))) return false;
    if ((bot.inventory.dirt ?? 0) < 2) await bot.relogin();
    if (!(await bot.pillarUp(base + 3, site.scaffold))) { await bot.digDown(site.scaffold); return false; }
    const out = [[1, 0], [-1, 0], [0, 1], [0, -1]].map(([dx, dy]) => ({ x: t.x + dx, y: t.y + dy, z: base + 4 }))
      .find(c => standable(world, site.inside, c.x, c.y, c.z));
    if (!out || !(await bot.go([out]))) { await bot.digDown(site.scaffold); return false; }
    await bot.settle();
    for (const z of [base + 2, base + 1]) {
      const d = { x: t.x, y: t.y, z };
      if (world.onServer(d.x, d.y, d.z) === "dirt" && (await bot.dig(d)) === "ok") site.scaffold.delete(cellOf(d));
    }
    return (await bot.place(hatch, { fine: true, everyone: site.everyone })) === "ok";
  }

  async function job(bot, b, via) {
    const k = cellOf(b);
    site.claimed.set(k, bot);
    site.standing.set(bot, via.stand ? cellOf(via.stand) : cellOf(via.g));
    try {
      let t = now();
      const tJob = t;
      if (!(await bot.go(via.path))) { spent.walk += now() - t; because("could not get there"); fail(k, b); return 0; }
      spent.walk += now() - t; t = now();
      if (via.h !== undefined && !(await bot.pillarUp(via.h, site.scaffold))) { because("could not pillar up"); fail(k, b); await bot.digDown(site.scaffold); spent.scaffold += now() - t; return 0; }
      spent.scaffold += now() - t; t = now();
      await bot.settle();
      const n = await burst(bot, b);
      spent.burst += now() - t; spent.bursts++; spent.blocks += n;
      if (process.env.BUILDER_DEBUG === bot.name) log(`  ${bot.name}: walked ${via.path.length} cells in ${((t - tJob) / 1000).toFixed(1)} s, burst ${n} in ${((now() - t) / 1000).toFixed(1)} s, ended: ${bot.burstEnd}`);
      return n;
    } finally {
      site.standing.delete(bot);
      if (site.claimed.get(k) === bot && !world.unconfirmed.has(k)) site.claimed.delete(k);
    }
  }

  const crews = [];
  async function run(bot) {
    let idleSince = null, nextHelp = 0;
    while (!finished) {
      await sleep(15);
      if (!bot.room || !bot.placed || bot.dead) { await sleep(300); continue; }
      if (pending.size === 0) break;
      await bot.settle();
      const tSearch = now();
      const cands = candidates(bot);
      // the band is the hatch: the room is done, and its builder climbs out
      const hatch = cands.find(b => b.role === "hatch");
      if (hatch && cands.every(b => b.role === "hatch")) {
        if (await climb(bot, hatch)) { bot.lastProgress = now(); continue; }
        log(`[${label}] ${bot.name}: could not climb out at ${cellOf(hatch)}; on by the stairs`);
      }
      let via = null, target = null;
      const room = how === "place" ? [...(laneOf.get(bot) ?? [])].find(z => z.startsWith("room") && climber.get(z) === bot) : null;
      const best = cands.length ? site.bestStand(bot, cands, how, room) : null;
      if (best) { via = best; target = best.b; }
      else for (const b of cands.slice(0, 6)) cool.set(cellOf(b), now() + 2500 + Math.random() * 2000);
      if (via && via.path.length && site.onScaffold(bot)) { status.set(bot, "taking its scaffolding down"); await bot.digDown(site.scaffold); continue; }
      if (!via && site.onScaffold(bot)) { status.set(bot, "taking its scaffolding down"); await bot.digDown(site.scaffold); continue; }
      // out of every block the band needs: back as a new guest with full stacks, where it stands
      if (!via && how === "place" && !cands.length && candidates(bot, { stock: false }).length) {
        status.set(bot, "fetching blocks");
        await bot.relogin();
        continue;
      }
      if (!via && how === "place" && cands.length && (bot.inventory.dirt ?? 0) < 4) { status.set(bot, "fetching dirt"); await bot.relogin(); continue; }
      if (!via && how === "place") for (const b of cands.slice(0, 6)) {
        const sc = site.scaffoldFor(bot, b, how);
        if (sc) { via = sc; target = b; break; }
        cool.set(cellOf(b), now() + 4000);
      }
      // set-aside blocks of its own zones: patched first, from scaffolding if need be, the structural ones before the rest
      if (!via && how === "place") {
        const c = bot.cell(), mine = laneOf.get(bot);
        const holes = [...aside].filter(k => pending.has(k) && !cooled(k) && !site.claimed.has(k)).map(k => byKey.get(k))
          .filter(b => b.role !== "hatch" && (!mine || mine.has(b.zone)) && anchored(b) && (bot.inventory[b.kind] ?? 0) > 0)
          .sort((a, b) => (STRUCTURAL.has(b.role) - STRUCTURAL.has(a.role)) * 100 + Math.hypot(a.x - c.x, a.y - c.y, a.z - c.z) - Math.hypot(b.x - c.x, b.y - c.y, b.z - c.z)).slice(0, 6);
        for (const b of holes) {
          const s = site.stand(bot, b, how) ?? site.scaffoldFor(bot, b, how);
          if (s && s.path.length <= 16) { via = s; target = b; aside.delete(cellOf(b)); tries.delete(cellOf(b)); break; }
          cool.set(cellOf(b), now() + 8000);
        }
      }
      // nothing in its own strip: a hand with the next strip's band, if it is near
      if (!via && how === "place" && now() >= nextHelp) {
        const c = bot.cell();
        for (const other of bots) {
          if (other === bot) continue;
          const theirs = candidates(bot, { lane: laneOf.get(other), bandOf: other })
            .filter(b => Math.abs(b.x - c.x) <= 6 && Math.abs(b.y - c.y) <= 6 && Math.abs(b.z - c.z) <= 3).slice(0, 6);
          for (const b of theirs) { const s = site.stand(bot, b, how); if (s && s.path.length <= 8) { via = s; target = b; break; } }
          if (via) break;
        }
        if (!via) nextHelp = now() + 4000;
      }
      if (!via) {
        spent.idle += 300;
        status.set(bot, "waiting");
        idleSince ??= now();
        bot.idleSince = idleSince;
        // ten seconds with nothing it can reach in its band: that part of the band is set aside, and it goes on
        const mates = bots.filter(o => [...(laneOf.get(o) ?? [])].some(z => laneOf.get(bot)?.has(z)));
        if (now() - idleSince > 6000 && mates.every(o => o.idleSince && now() - o.idleSince > 6000)) {
          const low = how === "place" ? band(bot) : null;
          let n = 0;
          // a block the ones above it stand on (a corner, where walls meet, a slab's corner) waits longer: set aside,
          // the whole column over it would go too
          const longWait = now() - idleSince > 20000;
          for (const k of keysIn(laneOf.get(bot))) {
            if (!pending.has(k) || aside.has(k)) continue;
            const b = byKey.get(k);
            if (how === "place" && b.step !== low) continue;
            if (!longWait && STRUCTURAL.has(b.role)) continue;
            aside.add(k); n++;
          }
          if (n && !quiet) log(`[${label}] ${bot.name}: nothing in reach of step ${low} of its zone, ${n} set aside: ${[...aside].slice(-Math.min(n, 4)).map(k => { const b = byKey.get(k); return `${b.x - site.x0},${b.y - site.y0},${b.z} ${b.role}`; }).join("; ")}`);
          // the clock starts again only once something was set aside: the structural ones need it to run on to 20 s
          if (n) idleSince = null;
          // all that is left anywhere is set aside, and none of it can be patched: this crew is done
          if (!n && [...pending].every(k => aside.has(k))) { if (now() - (bot.lastProgress ?? 0) > 30000) break; }
        }
        await sleep(300);
        continue;
      }
      idleSince = null; bot.idleSince = null;
      status.set(bot, `${via.h !== undefined ? "scaffolding for" : how === "place" ? "placing" : "clearing"} ${target.role ?? target.kind}`);
      const n = await job(bot, target, via);
      if (n) bot.lastProgress = now();
    }
    if (site.onScaffold(bot)) await bot.digDown(site.scaffold);
    status.set(bot, "done");
  }

  const watch = every(5000, () => {
    world.sweep(now());
    for (const b of bots) if (now() - (b.lastProgress ?? now()) > 40000 && b.path) { b.abandon(); b.lastProgress = now(); }
  });
  const report = every(30000, () => {
    const counts = {};
    for (const b of bots) { const s = (status.get(b) ?? "idle").split(" ")[0]; counts[s] = (counts[s] ?? 0) + 1; }
    log(`[${label}] ${total - pending.size}/${total}${aside.size ? `, ${aside.size} set aside` : ""}${site.scaffold.size ? `, scaffolding ${site.scaffold.size}` : ""}; ${Object.entries(counts).map(([s, n]) => `${n} ${s}`).join(", ")}` +
      (Object.keys(why).length ? `; misses: ${Object.entries(why).map(([r, n]) => `${r} ${n}`).join(", ")}` : ""));
    for (const r in why) delete why[r];
    const all = spent.search + spent.walk + spent.scaffold + spent.burst + spent.idle || 1, pc = v => `${Math.round(100 * v / all)}%`;
    if (spent.bursts) log(`[${label}]   time: searching ${pc(spent.search)}, walking ${pc(spent.walk)}, scaffolding ${pc(spent.scaffold)}, placing ${pc(spent.burst)}, waiting ${pc(spent.idle)}; ${(spent.blocks / spent.bursts).toFixed(1)} blocks a burst`);
    for (const k in spent) spent[k] = 0;
  });
  for (const b of bots) b.lastProgress = now();
  await Promise.all(bots.map(run));
  finished = true;
  clearInterval(watch); clearInterval(report);
  world.hooks.delete(hook);
  if (how === "dig") site.pendingDig = null; else site.pendingPlace = null;
  return [...pending].map(k => byKey.get(k));
}

/**
 * The repair crew: for every block still not right, straight in or from scaffolding; and if a neighbour hides it
 * (a cabinet against a wall, the walls either side of a corner), that neighbour comes out, the hole is filled
 * through the gap, and the neighbour goes back.
 */
export async function repair(site, plan, { label = "repair" } = {}) {
  const { world, bots } = site;
  const want = new Map(plan.map(b => [cellOf(b), b]));
  const wrong = b => world.onServer(b.x, b.y, b.z) !== b.kind;
  const busy = new Set();
  const stats = { straight: 0, through: 0, broken: 0 };

  async function reachAndDo(bot, b, how) {
    await bot.settle();
    const via = site.stand(bot, b, how) ?? site.scaffoldFor(bot, b, how);
    if (!via) return false;
    if (!(await bot.go(via.path))) return false;
    if (via.h !== undefined && !(await bot.pillarUp(via.h, site.scaffold))) { await bot.digDown(site.scaffold); return false; }
    await bot.settle();
    let r = how === "place" ? await bot.place(b, { fine: true, everyone: site.everyone }) : await bot.dig(b);
    if (how === "place" && r === "ok") { const until = now() + 1500; while (wrong(b) && now() < until) await sleep(30); r = wrong(b) ? "refused" : "ok"; }
    if (site.onScaffold(bot)) await bot.digDown(site.scaffold);
    return r === "ok";
  }

  function gapFor(bot, b) {
    const here = bot.cell();
    if (!standable(world, site.inside, here.x, here.y, here.z)) return null;
    for (const [dx, dy, dz] of DIRS) {
      const d = { x: b.x + dx, y: b.y + dy, z: b.z + dz };
      if (!site.inside(d.x, d.y) || d.z < 0 || !world.solid(d.x, d.y, d.z)) continue;
      const back = want.get(cellOf(d)) ?? { ...d, kind: world.onServer(d.x, d.y, d.z) };
      const k = cellOf(d), saved = world.overrides.get(k);
      world.overrides.set(k, "air");
      const r = reach(world, site.inside, here);
      const can = (t, s) => !bodyIn(s, t) && aimPlace(world, eyeOfCell(s), t);
      const anyStand = (t, rr) => { for (let x = t.x - 5; x <= t.x + 5; x++) for (let y = t.y - 5; y <= t.y + 5; y++) for (let z = t.z - 6; z <= t.z + 3; z++) if (rr.dist.has(key(x, y, z)) && can(t, { x, y, z })) return true; return false; };
      let through = anyStand(b, r), after = false;
      if (through) { world.overrides.set(cellOf(b), b.kind); after = anyStand(back, reach(world, site.inside, here)); world.overrides.delete(cellOf(b)); }
      if (saved === undefined) world.overrides.delete(k); else world.overrides.set(k, saved);
      if (through && after) return { d, back };
    }
    return null;
  }

  for (let pass = 1; pass <= 3; pass++) {
    const holes = plan.filter(wrong).sort((a, b) => rank(a) - rank(b));
    log(`[${label}] pass ${pass}: ${holes.length} to fix`);
    if (!holes.length) break;
    const queue = [...holes];
    await Promise.all(bots.map(async bot => {
      while (queue.length) {
        if (!bot.room) { await sleep(300); continue; }
        const c = bot.cell();
        queue.sort((a, b) => Math.hypot(a.x - c.x, a.y - c.y, (a.z - c.z) * 2) - Math.hypot(b.x - c.x, b.y - c.y, (b.z - c.z) * 2));
        const i = queue.findIndex(h => ![...busy].some(o => Math.abs(o.x - h.x) + Math.abs(o.y - h.y) + Math.abs(o.z - h.z) < 3));
        if (i < 0) { await sleep(300); continue; }
        const b = queue.splice(i, 1)[0];
        if (!wrong(b)) continue;
        busy.add(b);
        try {
          if ((bot.inventory[b.kind] ?? 0) <= 0) await bot.relogin();
          if (world.solid(b.x, b.y, b.z) && wrong(b)) { if (await reachAndDo(bot, b, "dig")) stats.broken++; else continue; }
          if (await reachAndDo(bot, b, "place")) { stats.straight++; continue; }
          const gap = gapFor(bot, b);
          if (!gap) continue;
          if (!(await reachAndDo(bot, gap.d, "dig"))) continue;
          const ok = await reachAndDo(bot, b, "place");
          for (let t = 0; t < 3 && wrong(gap.back) && !(await reachAndDo(bot, gap.back, "place")); t++) await sleep(300);
          if (ok) stats.through++;
        } catch (e) { log(`[${label}] ${bot.name}: ${e.message}`); }
        finally { busy.delete(b); }
      }
    }));
  }
  return stats;
}
