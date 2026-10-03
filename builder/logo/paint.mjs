// A crew of five that plays by the browser client's rules: it walks with the client's physics (web/physics.js),
// climbs only by jumping a block at a time, never falls more than three, and places or breaks a block only
// where the client's own ray (raycastBlocks in web/voxels.js, 4.5 blocks from the eyes) would hit it.
// Eight of them paint the PlayServ logo on a wall 64 blocks high.
// A foreman watches them: he hands out scaffolding when nobody can reach, and sets aside what cannot be built.
// Run: ROOM=green node --experimental-websocket builder/logo/paint.mjs [--probe]  (builder/logo/README.md)
import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { createBody, tick as physicsTick } from "../../web/physics.js";
import * as S from "../../web/spec.js";

// The browser client's settings (web/config.js): the API, the public client key, the room type.
const CONFIG = (() => {
  const window = {};
  new Function("window", readFileSync(fileURLToPath(new URL("../../web/config.js", import.meta.url)), "utf8"))(window);
  if (!window.CUBEWORLD?.clientKey) throw new Error("web/config.js has no CUBEWORLD.clientKey (copy web/config.example.js)");
  return window.CUBEWORLD;
})();
const API = CONFIG.api, KEY = CONFIG.clientKey, SLUG = CONFIG.slug ?? "cubeworld";
const PROBE = process.argv.includes("--probe");
const COUNT = PROBE ? 1 : process.env.STRIPS ? process.env.STRIPS.split(",").length : Number(process.env.COUNT ?? 8);
const PLACE_GAP_MS = 250;   // a held right button repeats every 4 ticks; a little slower than that

const sleep = ms => new Promise(r => setTimeout(r, ms));
const log = (...a) => console.log(new Date().toISOString().slice(11, 19), ...a);

// ── the world as the bots know it (server coords: x, y across, z up) ────────────────────────────
const WIDTH = 72, DEPTH = 48, MIN_Z = -4, MAX_Z = 64;   // three regions across, two rows (World.cs)
const LAYERS = { [-4]: "bedrock", [-3]: "dirt", [-2]: "dirt", [-1]: "grass" };
const TREES = [0, 1, 2, 3, 4, 5].flatMap(r => { const x0 = r % 3 * 24, y0 = Math.floor(r / 3) * 24; return [[x0 + 4, y0 + 5], [x0 + 18, y0 + 4], [x0 + 6, y0 + 18], [x0 + 19, y0 + 17]]; });
const terrain = new Map();
for (const [tx, ty] of TREES) {
  for (let dz = 0; dz < 5; dz++) terrain.set(`${tx}:${ty}:${dz}`, "wood");
  for (let dx = -2; dx <= 2; dx++) for (let dy = -2; dy <= 2; dy++) {
    const corner = Math.abs(dx) === 2 && Math.abs(dy) === 2, trunk = dx === 0 && dy === 0;
    const add = (z) => { if (!terrain.has(`${tx + dx}:${ty + dy}:${z}`)) terrain.set(`${tx + dx}:${ty + dy}:${z}`, "leaves"); };
    for (let dz = 3; dz <= 4; dz++) if (!corner && !trunk) add(dz);
    if (Math.abs(dx) <= 1 && Math.abs(dy) <= 1) add(5);
    if (Math.abs(dx) + Math.abs(dy) <= 1) add(6);
  }
}
const overrides = new Map();
const key = (x, y, z) => `${x}:${y}:${z}`;
const inside = (x, y, z) => x >= 0 && x < WIDTH && y >= 0 && y < DEPTH && z >= MIN_Z && z < MAX_Z;
function kindAt(x, y, z) {
  if (!inside(x, y, z)) return "air";
  const k = key(x, y, z);
  if (overrides.has(k)) return overrides.get(k);
  return LAYERS[z] ?? terrain.get(k) ?? "air";
}
const solid = (x, y, z) => kindAt(x, y, z) !== "air";
function hear(cube, op) { overrides.set(key(cube.x, cube.y, cube.z), op === "delete" ? (LAYERS[cube.z] ?? terrain.get(key(cube.x, cube.y, cube.z)) ?? "air") : cube.kind); }

// The client's view of the same world: x and z on the ground, y up.
const clientWorld = { isSolid: (x, y, z) => solid(x, z, y) };
// As VoxelWorld.isSolidForPhysics: the world's border and floor are walls.
const isSolidForPhysics = (x, y, z) => (x < 0 || x >= WIDTH || z < 0 || z >= DEPTH || y < MIN_Z) ? true : solid(x, z, y);

// raycastBlocks from web/voxels.js, as it is (voxels.js itself needs three.js, so it is copied here).
function raycastBlocks(world, origin, direction, reach) {
  let x = Math.floor(origin.x), y = Math.floor(origin.y), z = Math.floor(origin.z);
  const step = [Math.sign(direction.x), Math.sign(direction.y), Math.sign(direction.z)];
  const delta = [Math.abs(1 / direction.x), Math.abs(1 / direction.y), Math.abs(1 / direction.z)];
  const next = [
    direction.x > 0 ? (x + 1 - origin.x) * delta[0] : (origin.x - x) * delta[0],
    direction.y > 0 ? (y + 1 - origin.y) * delta[1] : (origin.y - y) * delta[1],
    direction.z > 0 ? (z + 1 - origin.z) * delta[2] : (origin.z - z) * delta[2],
  ];
  let face = null, travelled = 0;
  for (let i = 0; i < 64; i++) {
    if (world.isSolid(x, y, z) && face) return { x, y, z, nx: face[0], ny: face[1], nz: face[2], distance: travelled };
    const axis = next[0] < next[1] ? (next[0] < next[2] ? 0 : 2) : (next[1] < next[2] ? 1 : 2);
    travelled = next[axis];
    if (travelled > reach) return null;
    next[axis] += delta[axis];
    if (axis === 0) x += step[0]; else if (axis === 1) y += step[1]; else z += step[2];
    face = [0, 0, 0];
    face[axis] = -step[axis];
  }
  return null;
}

// The ray from an eye (server coords) to a point: what the client would hit, in server coords.
function rayHit(eye, point) {
  const d = { x: point.x - eye.x, y: point.z - eye.z, z: point.y - eye.y };
  const len = Math.hypot(d.x, d.y, d.z);
  if (len < 1e-6) return null;
  const hit = raycastBlocks(clientWorld, { x: eye.x, y: eye.z, z: eye.y }, { x: d.x / len, y: d.y / len, z: d.z / len }, S.BLOCK_REACH);
  return hit && { x: hit.x, y: hit.z, z: hit.y, nx: hit.nx, ny: hit.nz, nz: hit.ny, yaw: Math.atan2(-d.x, d.z), pitch: -Math.asin(d.y / len) };
}

// Points on the face of block b that faces direction n, the centre first.
function facePoints(b, n) {
  const c = { x: b.x + 0.5 + n[0] * 0.5, y: b.y + 0.5 + n[1] * 0.5, z: b.z + 0.5 + n[2] * 0.5 };
  const axes = [[1, 0, 0], [0, 1, 0], [0, 0, 1]].filter(a => a[0] * n[0] + a[1] * n[1] + a[2] * n[2] === 0);
  const pts = [c];
  for (const s of [0.35, -0.35]) for (const a of axes) pts.push({ x: c.x + a[0] * s, y: c.y + a[1] * s, z: c.z + a[2] * s });
  return pts;
}
const DIRS = [[0, 0, -1], [1, 0, 0], [-1, 0, 0], [0, 1, 0], [0, -1, 0], [0, 0, 1]];

// ── the site ────────────────────────────────────────────────────────────────────────────────────
const COLORS = ["red", "blue", "green", "yellow", "purple", "pink"];   // World.RegionColors
const REGION = COLORS.indexOf(process.env.ROOM ?? "green");
const R = 24 * (REGION % 3), RY = 24 * Math.floor(REGION / 3);   // the region's first column and row
const inRegion = (x, y) => x >= R && x < R + 24 && y >= RY && y < RY + 24;

// The picture: 24 wide (the whole region) and 64 high (z 0..63, all the world has), standing on the east edge
// of the world (x = 71) and facing west, over the whole map. Looking east in the client, the right hand is +y
// (camera right = forward × up), so the picture reads from y = 0 to y = 23; a wall read along +x from the south
// comes out mirrored, as the last one did. The PlayServ mark (playserv-dashboard/public/logo-small.svg) on top, "PLAY" and
// "SERV" under it. There is no white or violet block: the mark is gold, the letters sand.
const WALL_X = R + 23, W = 24, H = 64;
// Column u of the picture (0 = its left edge as seen from the west), d blocks in front of the wall.
const wallCell = (u, z, d = 0) => ({ x: WALL_X - d, y: RY + u, z });
const columnOf = c => c.y - RY, depthOf = c => WALL_X - c.x;

// The mark's three shapes, in its own 36 × 36 box, y down.
const MARK = [
  [[8.16281, 28.2395], [16.9535, 28.2395], [16.9535, 36], [0, 36], [0, 7.76051], [8.16281, 7.76051]],
  [[36, 20.4791], [27, 28.2395], [16.9535, 28.2395], [16.9535, 20.4791], [27, 20.4791], [27, 7.76051], [36, 7.76051]],
  [[27, 7.76051], [8.16281, 7.76051], [8.16281, 0], [27, 0]],
];
function inPolygon(px, py, poly) {
  let inside = false;
  for (let i = 0, j = poly.length - 1; i < poly.length; j = i++) {
    const [xi, yi] = poly[i], [xj, yj] = poly[j];
    if ((yi > py) !== (yj > py) && px < (xj - xi) * (py - yi) / (yj - yi) + xi) inside = !inside;
  }
  return inside;
}
// A 5 × 7 font for the eight letters, drawn twice as tall.
const FONT = {
  P: ["####.", "#...#", "#...#", "####.", "#....", "#....", "#...."],
  L: ["#....", "#....", "#....", "#....", "#....", "#....", "#####"],
  A: [".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
  Y: ["#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."],
  S: [".####", "#....", "#....", ".###.", "....#", "....#", "####."],
  E: ["#####", "#....", "#....", "####.", "#....", "#....", "#####"],
  R: ["####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"],
  V: ["#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."],
};

function paintingPlan() {
  const grid = new Map();                                   // "i:z" -> kind, i = 0..23 across
  const set = (i, z, kind) => grid.set(`${i}:${z}`, kind);
  // Background bands: stone behind the mark, brick behind PLAY, wood behind SERV; wood borders top and bottom.
  for (let i = 0; i < W; i++) for (let z = 0; z < H; z++)
    set(i, z, z === 0 || z === H - 1 ? "wood" : z >= 38 ? "stone" : z >= 21 ? "brick" : "wood");
  // The mark, 22 × 22, z 40..61.
  const S = 22;
  for (let a = 0; a < S; a++) for (let b = 0; b < S; b++) {
    const px = (a + 0.5) * 36 / S, py = (b + 0.5) * 36 / S;
    if (MARK.some(p => inPolygon(px, py, p))) set(1 + a, 61 - b, "gold");
  }
  // The words, letters 5 wide and 14 high with a gap of one: PLAY at z 23..36, SERV at z 4..17.
  const word = (text, top) => [...text].forEach((ch, n) => FONT[ch].forEach((row, r) => [...row].forEach((c, col) => {
    if (c === "#") for (const dz of [0, 1]) set(1 + n * 6 + col, top - r * 2 - dz, "sand");
  })));
  word("PLAY", 36);
  word("SERV", 17);
  for (let i = 0; i < W; i++) { const c = wallCell(i, -1); if (!solid(c.x, c.y, -1)) set(i, -1, "grass"); }
  return [...grid].map(([k, kind]) => { const [i, z] = k.split(":").map(Number); return { ...wallCell(i, z), kind }; })
    .filter(b => kindAt(b.x, b.y, b.z) !== b.kind);
}
// For looking at it in the terminal: the picture as the south sees it.
function asText(plan) {
  const m = new Map(plan.map(b => [`${columnOf(b)}:${b.z}`, b.kind]));
  const ch = { stone: ".", brick: ":", wood: "=", gold: "#", sand: "o", grass: "g" };
  const rows = [];
  for (let z = H - 1; z >= 0; z--) { let s = ""; for (let u = 0; u < W; u++) s += ch[m.get(`${u}:${z}`)] ?? " "; rows.push(s); }
  return rows.join("\n");
}

// ── getting about ───────────────────────────────────────────────────────────────────────────────
const air2 = (x, y, z) => !solid(x, y, z) && !solid(x, y, z + 1);
const standable = (x, y, z) => inRegion(x, y) && solid(x, y, z - 1) && air2(x, y, z);

// Where a body can go from a cell in one move: walk level, jump one up, or walk off and land at most three down.
function moves(c) {
  const out = [];
  for (const [dx, dy] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) {
    const x = c.x + dx, y = c.y + dy;
    if (!inRegion(x, y)) continue;
    if (standable(x, y, c.z)) { out.push({ x, y, z: c.z }); continue; }
    if (standable(x, y, c.z + 1) && !solid(c.x, c.y, c.z + 2)) { out.push({ x, y, z: c.z + 1 }); continue; }
    if (!air2(x, y, c.z)) continue;
    let z = c.z;
    while (z > c.z - 4 && !solid(x, y, z - 1)) z--;
    if (z >= c.z - 3 && solid(x, y, z - 1)) out.push({ x, y, z });
  }
  return out;
}

// Every cell reachable from a cell, with its distance and the way there.
function reach(from) {
  const dist = new Map([[key(from.x, from.y, from.z), 0]]), prev = new Map();
  const queue = [from];
  for (let i = 0; i < queue.length; i++) {
    const c = queue[i], d = dist.get(key(c.x, c.y, c.z));
    for (const n of moves(c)) {
      const k = key(n.x, n.y, n.z);
      if (dist.has(k)) continue;
      dist.set(k, d + 1); prev.set(k, c); queue.push(n);
    }
  }
  return { dist, prev, cells: queue };
}
function pathTo(r, to) {
  const path = [];
  for (let c = to; c; c = r.prev.get(key(c.x, c.y, c.z))) path.unshift(c);
  return path.slice(1);
}

// ── platform ────────────────────────────────────────────────────────────────────────────────────
async function api(method, path, token, body) {
  const headers = { "X-PlayServ-Client": KEY, "Content-Type": "application/json" };
  if (token) headers.Authorization = `Bearer ${token}`;
  const res = await fetch(`${API}${path}`, { method, headers, body: body && JSON.stringify(body) });
  const json = await res.json().catch(() => ({}));
  if (!res.ok) throw new Error(`${method} ${path} → ${res.status} ${json.code || json.title || ""}`);
  return json;
}

class Bot {
  constructor(name) { this.name = name; this.inventory = {}; this.placed = 0; this.dug = 0; this.yaw = 0; this.pitch = 0; this.path = null; }

  async join(room) {
    this.player = await api("POST", "/auth/players/anon", null, { display_name: this.name });
    const ticket = await api("POST", `/rooms/${SLUG}/${room}:join`, this.player.access_token, {});
    const c = ticket.connect;
    const url = c ? `${c.transport === "wss" ? "wss" : "ws"}://${c.host}:${c.port}/` : `${API.replace(/^http/, "ws")}/games/${SLUG}`;
    this.socket = new WebSocket(url);
    await new Promise((resolve, reject) => {
      this.socket.onopen = () => this.socket.send(JSON.stringify({
        playerId: this.player.player_id, displayName: this.name, token: this.player.access_token, reservationToken: ticket.reservation_token,
      }));
      this.socket.onclose = e => { this.closed = true; reject(new Error(`${this.name}: closed ${e.code} ${e.reason}`)); log(this.name, "closed", e.code, e.reason); };
      this.socket.onmessage = e => this.onFrame(JSON.parse(e.data), resolve);
    });
    this.timer = setInterval(() => this.tick(), S.TICK_MS);
  }

  onFrame(f, resolve) {
    switch (f.type) {
      case "welcome":
        this.body = createBody(f.you.x, f.you.z, f.you.y);
        this.inventory = f.inventory;
        this.breakTicks = Object.fromEntries(f.blocks.map(b => [b.kind, b.breakTicks]));
        for (const c of f.world) hear(c, "upsert");
        this.send({ op: "bombs" });
        resolve?.(f);
        break;
      case "cube": hear(f.cube, f.op); if (this.optimistic === key(f.cube.x, f.cube.y, f.cube.z)) this.optimistic = null; break;
      case "cubes": for (const { op, cube } of f.changes) hear(cube, op); break;
      case "inventory": this.inventory = f.inventory; break;
      case "refused":
        this.inventory = f.inventory;
        if (this.optimistic) { overrides.set(this.optimistic, "air"); scaffold.delete(this.optimistic); this.optimistic = null; }
        break;
      case "death": if (f.player === this.player.player_id) { this.dead = true; log(this.name, "died"); setTimeout(() => this.send({ op: "respawn" }), 1500); } break;
      case "respawn": this.body = createBody(f.you.x, f.you.z, f.you.y); this.path = null; this.dead = false; this.arrived?.(false); break;
      case "hurt": if (f.player === this.player?.player_id && f.strength === 0 && !f.by) log(this.name, "hurt, health", f.health); break;
    }
  }

  send(m) { if (this.socket?.readyState === 1) this.socket.send(JSON.stringify(m)); }

  // Where the bot stands, as a cell (server coords), once it is on the ground.
  cell() {
    const b = this.body;
    return { x: Math.floor(b.x), y: Math.floor(b.z), z: Math.round(b.y) };
  }
  eye() { return { x: this.body.x, y: this.body.z, z: this.body.y + S.EYE_HEIGHT }; }

  // One client tick: steer along the path, run the client's physics, tell the server where the body is.
  tick() {
    const b = this.body;
    if (!b) return;
    const input = { forward: 0, strafe: 0, jump: false, sneak: false, sprint: false, yaw: this.yaw };
    if (this.jumpNow && b.onGround) { input.jump = true; this.jumpNow = false; }
    if (this.path?.length) {
      const w = this.path[0], next = this.path[1], from = this.from;
      const dx = w.x + 0.5 - b.x, dz = w.y + 0.5 - b.z, d = Math.hypot(dx, dz);
      // A turn, a drop, a jump or the end: come in slowly, so the 0.6 wide body stays inside its cell.
      const turning = !next || next.z !== w.z || next.x - w.x !== w.x - from.x || next.y - w.y !== w.y - from.y;
      if (d < (turning ? 0.12 : 0.3) && b.onGround && Math.abs(b.y - w.z) < 0.05) {
        this.from = this.path.shift();
        this.progress = 0;
        if (!this.path.length) { this.path = null; this.arrived?.(true); }
      } else {
        this.yaw = Math.atan2(-dx, dz);
        this.pitch = 0;
        input.forward = turning && d < 0.7 ? Math.max(0.12, d * 0.8) : 1;
        input.yaw = this.yaw;
        if (w.z > b.y + 0.5 && b.onGround && d < 1.4) input.jump = true;
        if (++this.progress > 80) { this.path = null; this.arrived?.(false); }
      }
    }
    physicsTick(b, input, isSolidForPhysics);
    const pose = `${b.x.toFixed(3)},${b.y.toFixed(3)},${b.z.toFixed(3)},${this.yaw.toFixed(3)},${this.pitch.toFixed(3)},${b.onGround}`;
    this.idleTicks = pose === this.lastPose ? (this.idleTicks ?? 0) + 1 : 0;
    if (pose !== this.lastPose || this.idleTicks % 20 === 0) {
      this.lastPose = pose;
      this.send({ op: "move", x: b.x, y: b.z, z: b.y, yaw: this.yaw, pitch: this.pitch, onGround: b.onGround, sneaking: false, sprinting: false });
    }
  }

  // Walks a path of cells; true once there, false if it got stuck on the way.
  go(path) {
    if (!path.length) return Promise.resolve(true);
    this.progress = 0;
    this.from = this.cell();
    return new Promise(resolve => { this.arrived = ok => { this.arrived = null; resolve(ok); }; this.path = [...path]; });
  }

  async settle() { for (let i = 0; i < 40 && !this.body.onGround; i++) await sleep(S.TICK_MS); }
  look(hit) { this.yaw = hit.yaw; this.pitch = hit.pitch; }
}

// ── the crew ────────────────────────────────────────────────────────────────────────────────────
// Blocks are claimed while a bot is on its way, cells while a bot stands in them or is heading there.
const claimed = new Map(), standing = new Map();
const cellOf = c => key(c.x, c.y, c.z);
const bodyIn = (s, b) => s.x === b.x && s.y === b.y && (b.z === s.z || b.z === s.z + 1);
// As World.Intersects on the server: a body's hitbox overlaps block b.
const overlaps = (body, b) => body.x - 0.3 < b.x + 1 && body.x + 0.3 > b.x && body.z - 0.3 < b.y + 1 && body.z + 0.3 > b.y && body.y < b.z + 1 && body.y + S.HEIGHT > b.z;
const EVERYONE = [];
let PLAN_KEYS = new Set();
const someoneIn = (bots, b) => EVERYONE.some(o => o.body && overlaps(o.body, b));

// How a bot at cell s would place b: the ray from its eyes hits a solid neighbour of b on the face towards b.
function placeFrom(s, b) {
  const eye = { x: s.x + 0.5, y: s.y + 0.5, z: s.z + S.EYE_HEIGHT };
  for (const [dx, dy, dz] of DIRS) {
    const a = { x: b.x + dx, y: b.y + dy, z: b.z + dz };
    if (!solid(a.x, a.y, a.z)) continue;
    const n = [-dx, -dy, -dz];
    for (const p of facePoints(a, n)) {
      const hit = rayHit(eye, p);
      if (hit && hit.x === a.x && hit.y === a.y && hit.z === a.z && hit.nx === n[0] && hit.ny === n[1] && hit.nz === n[2]) return p;
    }
  }
  return null;
}
// How a bot at s would dig b: the ray hits b itself, first.
function digFrom(s, b) {
  const eye = { x: s.x + 0.5, y: s.y + 0.5, z: s.z + S.EYE_HEIGHT };
  for (const n of DIRS) for (const p of facePoints(b, n)) {
    const hit = rayHit(eye, p);
    if (hit && hit.x === b.x && hit.y === b.y && hit.z === b.z) return p;
  }
  return null;
}

// The next job for a bot: the nearest block it can walk to a place for and reach from there.
function chooseJob(bot, bots, jobs, how, order, pendingKeys) {
  const here = bot.cell();
  if (!standable(here.x, here.y, here.z)) return null;
  const r = reach(here);
  const others = new Set(bots.filter(o => o !== bot).flatMap(o => [cellOf(o.cell()), standing.get(o)].filter(Boolean)));
  let best = null;
  for (const b of order(jobs.filter(j => !claimed.has(cellOf(j)))).slice(0, 40)) {
    if (someoneIn(bots, b)) continue;
    for (const s of r.cells) {
      if (Math.abs(s.x - b.x) > 5 || Math.abs(s.y - b.y) > 5 || Math.abs(s.z + 1.6 - b.z) > 5.5) continue;
      if (bodyIn(s, b) || others.has(cellOf(s))) continue;
      if (how === "place" && (pendingKeys.has(key(s.x, s.y, s.z)) || pendingKeys.has(key(s.x, s.y, s.z + 1)))) continue;
      const d = r.dist.get(cellOf(s));
      if (best && d >= best.d) continue;
      const point = how === "place" ? placeFrom(s, b) : digFrom(s, b);
      if (point) best = { b, s, d, r };
    }
    if (best) break;
  }
  return best && { block: best.b, stand: best.s, path: pathTo(best.r, best.s) };
}

// ── scaffolding ─────────────────────────────────────────────────────────────────────────────────
// Dirt a bot puts under itself to get up to a block it cannot reach, and takes away again on the way down.
const scaffold = new Set();
const onScaffold = bot => { const c = bot.cell(); return scaffold.has(key(c.x, c.y, c.z - 1)); };

// Pillar up as a player does: look straight down, jump, and at the top of the jump place a block where the feet were.
async function pillarUp(bot, height) {
  while (bot.cell().z < height) {
    await bot.settle();
    const c = bot.cell();
    if (solid(c.x, c.y, c.z + 2)) return false;
    if (!(await bot.go([c]))) return false;                       // stand in the middle of the cell
    await bot.settle();
    bot.pitch = Math.PI / 2;
    bot.jumpNow = true;
    const until = Date.now() + 1000;
    while (bot.body.y < c.z + 1.02 && Date.now() < until) await sleep(10);
    if (bot.body.y < c.z + 1.02) return false;
    const hit = rayHit(bot.eye(), { x: c.x + 0.5, y: c.y + 0.5, z: c.z });
    if (!hit || hit.x !== c.x || hit.y !== c.y || hit.z !== c.z - 1 || hit.nz !== 1) return false;
    const k = key(c.x, c.y, c.z);
    bot.send({ op: "place", x: hit.x, y: hit.y, z: hit.z, nx: hit.nx, ny: hit.ny, nz: hit.nz, kind: "dirt" });
    overrides.set(k, "dirt");                                     // as the client would draw it straight away
    bot.optimistic = k;
    scaffold.add(k);
    await sleep(100);
    await bot.settle();
    await sleep(150);
    if (kindAt(c.x, c.y, c.z) !== "dirt") { scaffold.delete(k); return false; }
  }
  bot.pitch = 0;
  return true;
}

// Dig the scaffolding out from under the feet, a block at a time, down to whatever it stands on.
async function digDown(bot) {
  while (onScaffold(bot)) {
    await bot.settle();
    const c = bot.cell(), below = { x: c.x, y: c.y, z: c.z - 1 };
    const hit = rayHit(bot.eye(), { x: c.x + 0.5, y: c.y + 0.5, z: c.z });
    if (!hit || hit.x !== below.x || hit.y !== below.y || hit.z !== below.z) return false;
    bot.look(hit);
    bot.send({ op: "dig", state: "start", x: below.x, y: below.y, z: below.z });
    const until = Date.now() + (bot.breakTicks.dirt ?? 15) * S.TICK_MS + 2500;
    while (solid(below.x, below.y, below.z) && Date.now() < until) await sleep(50);
    if (solid(below.x, below.y, below.z)) { bot.send({ op: "dig", state: "stop", ...below }); return false; }
    scaffold.delete(key(below.x, below.y, below.z));
    await sleep(S.DIG_COOLDOWN_TICKS * S.TICK_MS);
    await bot.settle();
  }
  bot.pitch = 0;
  return true;
}

// ── the foreman ─────────────────────────────────────────────────────────────────────────────────
// It hands out scaffolding when nobody can reach the next block, sets aside what cannot be built at all,
// takes a job back from a bot that has been at it too long, and says every half minute how things stand.
const foreman = {
  bot: null,
  blocked: new Set(),           // blocks nobody can get at, even from scaffolding: they no longer hold the layer up
  status: new Map(),            // bot -> what it is doing
  started: Date.now(),

  say(...a) { log("[foreman]", ...a); },

  // Scaffolding for a block nobody can reach: a column the bot can get to, pillar up it, and place from the top.
  scaffoldJob(bot, bots, candidates, planKeys, how = "place", onlyHere = false) {
    const here = bot.cell();
    if (!standable(here.x, here.y, here.z)) return null;
    const r = reach(here);
    // Columns other bots stand in or are heading for, and the ones next to them: one pillar to a column.
    const taken = new Set();
    for (const o of bots) if (o !== bot) for (const c of [o.cell(), ...[standing.get(o)].filter(Boolean).map(k => { const [x, y] = k.split(":").map(Number); return { x, y }; })])
      for (const [dx, dy] of [[0, 0], [1, 0], [-1, 0], [0, 1], [0, -1]]) taken.add(`${c.x + dx}:${c.y + dy}`);
    let best = null;
    for (const b of candidates.slice(0, 25)) {
      for (const g of onlyHere ? [here] : r.cells) {
        if (Math.abs(g.x - b.x) > 4 || Math.abs(g.y - b.y) > 4 || g.z > b.z || (!onlyHere && taken.has(`${g.x}:${g.y}`))) continue;
        for (let h = g.z + 1; h <= Math.min(MAX_Z - 2, b.z + 1); h++) {
          // Every block of the pillar, and the room for the body on top, must be free and outside the plan.
          let clear = true;
          for (let z = g.z; z <= h + 1 && clear; z++) if (solid(g.x, g.y, z) || planKeys.has(key(g.x, g.y, z)) || scaffold.has(key(g.x, g.y, z))) clear = false;
          if (!clear) break;
          const s = { x: g.x, y: g.y, z: h };
          if (bodyIn(s, b)) continue;
          const cost = r.dist.get(cellOf(g)) + 4 * (h - g.z);
          if (best && cost >= best.cost) break;
          if (h - g.z > (bot.inventory.dirt ?? 0)) break;
          if (how === "place" ? placeFrom(s, b) : digFrom(s, b)) { best = { b, g, h, cost, path: pathTo(r, g) }; break; }
        }
      }
      if (best) break;
    }
    return best;
  },

  // Every few seconds: a bot that has not got anything done for 40 s is told to drop what it is doing.
  watch(bots, claimedByKey) {
    for (const b of bots) {
      if (Date.now() - (b.lastProgress ?? this.started) > 40000 && b.path) {
        this.say(`${b.name} has been at ${this.status.get(b) ?? "something"} for 40 s with nothing done: dropping it`);
        b.path = null; b.arrived?.(false);
        for (const [k, o] of claimedByKey) if (o === b) claimedByKey.delete(k);
        b.lastProgress = Date.now();
      }
    }
  },

  report(bots, label, done, total) {
    const lines = bots.map(b => `${b.name}: ${this.status.get(b) ?? "idle"}`).join("; ");
    this.say(`${label} ${done}/${total}${this.blocked.size ? `, set aside ${this.blocked.size}` : ""}${scaffold.size ? `, scaffolding ${scaffold.size}` : ""} — ${lines}`);
  },

  // The foreman himself walks over to where the work is and watches, a few blocks back.
  async walkAbout(bots) {
    const me = this.bot;
    if (!me || me.path) return;
    const busy = bots.filter(b => this.status.get(b)?.startsWith("scaffold")) ;
    const focus = (busy.length ? busy : bots).map(b => b.body);
    const cx = focus.reduce((s, b) => s + b.x, 0) / focus.length, cy = focus.reduce((s, b) => s + b.z, 0) / focus.length;
    const here = me.cell();
    if (!standable(here.x, here.y, here.z)) return;
    const r = reach(here);
    const spot = r.cells.filter(c => c.z <= 2 && !PLAN_KEYS.has(cellOf(c)) && !PLAN_KEYS.has(key(c.x, c.y, c.z + 1))).sort((a, b) => Math.abs(Math.hypot(a.x - cx, a.y - cy) - 4) - Math.abs(Math.hypot(b.x - cx, b.y - cy) - 4))[0];
    if (spot && Math.hypot(spot.x - here.x, spot.y - here.y) > 2) await me.go(pathTo(r, spot));
    me.look({ yaw: Math.atan2(-(cx - me.body.x), cy - me.body.z), pitch: -0.3 });
  },
};

async function crew(bots, jobs, how, label) {
  const pending = new Set(jobs.map(cellOf));
  const tries = new Map();
  const total = jobs.length;
  const byKey = new Map(jobs.map(j => [cellOf(j), j]));
  const planKeys = new Set(jobs.map(cellOf));
  const open = () => jobs.filter(j => pending.has(cellOf(j)));
  const done = b => (how === "place" ? kindAt(b.x, b.y, b.z) === b.kind : !solid(b.x, b.y, b.z));
  const anchored = b => DIRS.some(([dx, dy, dz]) => solid(b.x + dx, b.y + dy, b.z + dz));
  const idleSince = new Map();

  // Placing goes up a layer at a time, only blocks with something to stand against; digging, the nearest first.
  const order = bot => list => {
    let l = list.filter(b => !foreman.blocked.has(cellOf(b)));
    if (how === "place") {
      l = l.filter(b => anchored(b) && (bot.inventory[b.kind] ?? 0) > 0 && (b.kind !== "sand" || solid(b.x, b.y, b.z - 1)));
      const low = Math.min(...open().filter(b => !foreman.blocked.has(cellOf(b)) && anchored(b)).map(b => b.z));
      l = l.filter(b => b.z <= low + 1);
    }
    const c = bot.cell();
    return l.sort((a, b) => (a.z - b.z) * (how === "place" ? 3 : -3) + Math.hypot(a.x - c.x, a.y - c.y) - Math.hypot(b.x - c.x, b.y - c.y));
  };

  // Aim from where the body actually is, as the client would, and place or dig.
  async function act(bot, b) {
    const eye = bot.eye();
    const points = how === "place"
      ? DIRS.flatMap(([dx, dy, dz]) => solid(b.x + dx, b.y + dy, b.z + dz) ? facePoints({ x: b.x + dx, y: b.y + dy, z: b.z + dz }, [-dx, -dy, -dz]).map(p => ({ p, a: { x: b.x + dx, y: b.y + dy, z: b.z + dz }, n: [-dx, -dy, -dz] })) : [])
      : DIRS.flatMap(n => facePoints(b, n).map(p => ({ p, a: b })));
    let hit = null;
    for (const { p, a, n } of points) {
      const h = rayHit(eye, p);
      if (h && h.x === a.x && h.y === a.y && h.z === a.z && (!n || (h.nx === n[0] && h.ny === n[1] && h.nz === n[2]))) { hit = h; break; }
    }
    if (!hit || done(b) || (how === "place" && someoneIn(bots, b))) return false;
    bot.look(hit);
    await sleep(2 * S.TICK_MS);
    if (how === "place") {
      bot.send({ op: "place", x: hit.x, y: hit.y, z: hit.z, nx: hit.nx, ny: hit.ny, nz: hit.nz, kind: b.kind });
      const until = Date.now() + 800;
      while (!done(b) && Date.now() < until) await sleep(50);
      await sleep(PLACE_GAP_MS);
    } else {
      bot.send({ op: "dig", state: "start", x: b.x, y: b.y, z: b.z });
      const until = Date.now() + (bot.breakTicks[kindAt(b.x, b.y, b.z)] ?? 200) * S.TICK_MS + 2500;
      while (!done(b) && Date.now() < until) await sleep(50);
      if (!done(b)) bot.send({ op: "dig", state: "stop", x: b.x, y: b.y, z: b.z });
      await sleep(S.DIG_COOLDOWN_TICKS * S.TICK_MS);
    }
    return done(b);
  }

  function fail(k, b) {
    if ((tries.set(k, (tries.get(k) ?? 0) + 1).get(k)) >= 8) {
      foreman.blocked.add(k);
      foreman.say(`setting ${k} ${b.kind ?? ""} aside: tried 8 times`);
    }
  }

  async function work(bot) {
    const here = () => bot.cell();
    bot.lastProgress = Date.now();
    while (pending.size && !bot.closed && !crewDone) {
      await sleep(20);
      for (const k of [...pending]) if (done(byKey.get(k))) pending.delete(k);
      if ([...pending].every(k => foreman.blocked.has(k))) break;
      await bot.settle();
      const candidates = order(bot)(open().filter(j => !claimed.has(cellOf(j))));
      let job = chooseJob(bot, bots, open(), how, order(bot), pending);
      let viaScaffold = null;
      if ((!job || job.path.length) && onScaffold(bot)) {
        // On scaffolding with nothing in reach: raise it if that reaches something, or else take it down.
        viaScaffold = foreman.scaffoldJob(bot, bots, candidates.filter(b => !someoneIn(bots, b)), planKeys, how, true);
        if (viaScaffold) { job = { block: viaScaffold.b, stand: { ...here() }, path: [] }; bot.waitedUp = 0; }
        else if ((bot.waitedUp = (bot.waitedUp ?? 0) + 1) < 16) { foreman.status.set(bot, "waiting on its scaffolding"); await sleep(500); continue; }
        else { bot.waitedUp = 0; foreman.status.set(bot, "taking its scaffolding down"); await digDown(bot); continue; }
      }
      if (!job) {
        viaScaffold = foreman.scaffoldJob(bot, bots, candidates.filter(b => !someoneIn(bots, b)), planKeys, how);
        if (viaScaffold) {
          job = { block: viaScaffold.b, stand: { ...viaScaffold.g }, path: viaScaffold.path };
          if (viaScaffold.h - viaScaffold.g.z > 2) foreman.say(`${bot.name}: scaffolding ${viaScaffold.h - viaScaffold.g.z} up at ${viaScaffold.g.x}:${viaScaffold.g.y} for ${cellOf(viaScaffold.b)} ${viaScaffold.b.kind}`);
        }
      }
      if (!job) {
        foreman.status.set(bot, "waiting for work");
        idleSince.set(bot, idleSince.get(bot) ?? Date.now());
        // Everyone has been waiting a minute: whatever is left in the lowest layer cannot be got at. Set it aside.
        if (bots.every(o => idleSince.has(o) && Date.now() - idleSince.get(o) > 60000)) {
          const rest = open().filter(b => !foreman.blocked.has(cellOf(b)) && (how !== "place" || anchored(b)));
          const low = Math.min(...rest.map(b => b.z));
          for (const b of rest.filter(b => b.z === low)) foreman.blocked.add(cellOf(b));
          foreman.say(`nobody can reach layer z=${low}: setting ${rest.filter(b => b.z === low).length} blocks aside`);
          for (const o of bots) idleSince.set(o, Date.now());
        }
        await sleep(500);
        continue;
      }
      idleSince.delete(bot);
      const k = cellOf(job.block), b = job.block;
      claimed.set(k, bot);
      standing.set(bot, cellOf(job.stand));
      foreman.status.set(bot, `${viaScaffold ? "scaffolding for" : how === "place" ? "placing" : "clearing"} ${k} ${b.kind ?? ""}`.trim());
      try {
        if (!(await bot.go(job.path))) { fail(k, b); await sleep(300); continue; }
        if (viaScaffold && !(await pillarUp(bot, viaScaffold.h))) { foreman.say(`${bot.name} could not pillar up at ${viaScaffold.g.x}:${viaScaffold.g.y}`); fail(k, b); await digDown(bot); continue; }
        await bot.settle();
        if (await act(bot, b)) {
          pending.delete(k);
          how === "place" ? bot.placed++ : bot.dug++;
          bot.lastProgress = Date.now();
        } else { fail(k, b); await sleep(150); }
      } finally {
        claimed.delete(k);
        standing.delete(bot);
      }
    }
    if (onScaffold(bot)) { foreman.status.set(bot, "taking its scaffolding down"); await digDown(bot); }
    foreman.status.set(bot, "done");
  }

  let crewDone = false;
  const watch = setInterval(() => foreman.watch(bots, claimed), 5000);
  const report = setInterval(() => foreman.report(bots, label, total - pending.size, total), 30000);
  const walk = setInterval(() => foreman.walkAbout(bots).catch(() => {}), 8000);
  await Promise.all(bots.map(work));
  crewDone = true;
  clearInterval(watch); clearInterval(report); clearInterval(walk);
  foreman.report(bots, label, total - pending.size, total);
  return open();
}

// ── main ────────────────────────────────────────────────────────────────────────────────────────
const probe = await api("POST", "/auth/players/anon", null, { display_name: "probe" });
const rooms = (await api("GET", `/rooms/${SLUG}:browse`, probe.access_token)).data;
log("rooms:", rooms.map(r => `${r.room_name} ${r.players}/${r.capacity}`).join(", "));
const room = rooms.find(r => r.room_name.startsWith(`${COLORS[REGION]}-`))?.room_name;
if (!room) throw new Error(`no ${COLORS[REGION]} room`);

const names = ["Bob the Builder", "Wendy", "Scoop", "Muck", "Dizzy", "Roley", "Lofty", "Pilchard"];
const bots = [];
for (let i = 0; i < COUNT; i++) {
  const bot = new Bot(PROBE ? "probe" : names[i]);
  await bot.join(room);
  bots.push(bot);
  await sleep(400);
}
EVERYONE.push(...bots);
if (!PROBE) {
  foreman.bot = new Bot("Foreman");
  await foreman.bot.join(room);
  EVERYONE.push(foreman.bot);
  foreman.say("on site");
}
log(bots.map(b => `${b.name} at ${JSON.stringify(b.cell())}`).join("; "));
const tally = list => Object.entries(list.reduce((m, b) => (m[b.kind] = (m[b.kind] ?? 0) + 1, m), {})).map(e => e.join(" ")).join(", ");

// RESTORE=x:y:z:kind puts back single blocks (someone else's, taken by mistake) and stops there.
if (process.env.RESTORE) {
  const blocks = process.env.RESTORE.split(",").map(t => { const [x, y, z, kind] = t.split(":"); return { x: +x, y: +y, z: +z, kind }; });
  const left = await crew(bots, blocks, "place", "restored");
  log(left.length ? `not restored: ${left.map(cellOf).join(" ")}` : `restored ${blocks.map(b => `${cellOf(b)}=${b.kind}`).join(" ")}`);
  EVERYONE.forEach(b => b.socket.close());
  process.exit(0);
}
const plan = paintingPlan();
PLAN_KEYS = new Set(plan.map(cellOf));
log("painting:", plan.length, "blocks;", tally(plan));
const inWay = [];
for (let u = 0; u < W; u++) for (let z = 0; z < H; z++) for (const d of [0, 1]) {
  const c = wallCell(u, z, d);
  if (solid(c.x, c.y, z) && !PLAN_KEYS.has(cellOf(c))) inWay.push(`${cellOf(c)}=${kindAt(c.x, c.y, z)}`);
}
log("in the way:", inWay.join(" ") || "nothing");
if (PROBE) { console.log(asText(paintingPlan())); bots.forEach(b => b.socket.close()); process.exit(0); }

// ── strips ──────────────────────────────────────────────────────────────────────────────────────
// Each worker paints a strip three columns wide from the bottom up, from a dirt pillar in front of it that
// it raises a block for every row. The foreman keeps an eye on every strip.
const ONLY = process.env.STRIPS ? process.env.STRIPS.split(",").map(Number) : bots.map((_, k) => k);
const STRIPS = ONLY.map((k, i) => ({ k, i, us: [3 * k, 3 * k + 1, 3 * k + 2] }));
const byCell = new Map(plan.map(b => [cellOf(b), b]));
const wanted = (u, z) => byCell.get(cellOf(wallCell(u, z)));
const painted = b => kindAt(b.x, b.y, b.z) === b.kind;
// Dirt left standing in front of the wall by earlier runs counts as scaffolding: it comes down at the end.
for (const [k, kind] of overrides) {
  const [x, y, z] = k.split(":").map(Number);
  if (kind === "dirt" && inRegion(x, y) && depthOf({ x }) >= 1 && depthOf({ x }) <= 5 && z >= 0) scaffold.add(k);
}
log("old scaffolding blocks:", scaffold.size);

// A column in front of the strip with nothing in it from the ground up, for a new pillar.
function freeColumn(strip, bot) {
  const others = new Set(bots.filter(o => o !== bot).map(o => o.pillar).filter(Boolean));
  for (const d of [1, 2, 3])
    for (const u of [strip.us[1], strip.us[0], strip.us[2], strip.us[0] - 1, strip.us[2] + 1]) {
      const { x, y } = wallCell(u, 0, d);
      if (!inRegion(x, y)) continue;
      if (others.has(`${x}:${y}`)) continue;
      let free = true;
      for (let z = 0; z < H && free; z++) if (solid(x, y, z)) free = false;
      if (free && standable(x, y, 0)) return { x, y, z: 0 };
    }
  return null;
}

// Place one block of the picture from where the body is, as the client would: the ray to a face of a neighbour.
async function digOne(bot, o) {
  const eye = bot.eye();
  let hit = null;
  for (const n of DIRS) for (const p of facePoints(o, n)) { const h = rayHit(eye, p); if (!hit && h && h.x === o.x && h.y === o.y && h.z === o.z) hit = h; }
  if (!hit) return false;
  bot.look(hit);
  await sleep(2 * S.TICK_MS);
  bot.send({ op: "dig", state: "start", x: o.x, y: o.y, z: o.z });
  const until = Date.now() + (bot.breakTicks[kindAt(o.x, o.y, o.z)] ?? 200) * S.TICK_MS + 2000;
  while (solid(o.x, o.y, o.z) && Date.now() < until) await sleep(40);
  if (solid(o.x, o.y, o.z)) { bot.send({ op: "dig", state: "stop", x: o.x, y: o.y, z: o.z }); return false; }
  bot.dug++;
  await sleep(S.DIG_COOLDOWN_TICKS * S.TICK_MS);
  return true;
}

async function placeOne(bot, b) {
  for (let attempt = 0; attempt < 4; attempt++) {
    if (painted(b)) return true;
    if (solid(b.x, b.y, b.z)) { await digOne(bot, b); continue; }
    if (b.kind === "sand" && !solid(b.x, b.y, b.z - 1)) return false;
    const eye = bot.eye();
    let hit = null;
    for (const [dx, dy, dz] of DIRS) {
      const a = { x: b.x + dx, y: b.y + dy, z: b.z + dz };
      if (!solid(a.x, a.y, a.z)) continue;
      const n = [-dx, -dy, -dz];
      for (const p of facePoints(a, n)) {
        const h = rayHit(eye, p);
        if (h && h.x === a.x && h.y === a.y && h.z === a.z && h.nx === n[0] && h.ny === n[1] && h.nz === n[2]) { hit = h; break; }
      }
      if (hit) break;
    }
    if (!hit) return false;
    bot.look(hit);
    await sleep(2 * S.TICK_MS);
    bot.send({ op: "place", x: hit.x, y: hit.y, z: hit.z, nx: hit.nx, ny: hit.ny, nz: hit.nz, kind: b.kind });
    const until = Date.now() + 900;
    while (!painted(b) && Date.now() < until) await sleep(40);
    if (painted(b)) { bot.placed++; bot.lastProgress = Date.now(); await sleep(PLACE_GAP_MS); return true; }
    await sleep(300);
  }
  return false;
}

async function paintStrip(bot, strip) {
  bot.lastProgress = Date.now();
  for (let z = -1; z < H; z++) {
    const row = strip.us.map(u => wanted(u, z)).filter(b => b && !painted(b) && !foreman.blocked.has(cellOf(b)));
    if (!row.length) continue;
    strip.row = z;
    for (let tries = 0; ; tries++) {
      await bot.settle();
      // Up to the row: on its pillar, with the eyes above the top of the block the row is placed on.
      const c0 = bot.cell();
      const atStrip = columnOf(c0) >= strip.us[0] - 1 && columnOf(c0) <= strip.us[2] + 1 && depthOf(c0) >= 1 && depthOf(c0) <= 3;
      if (!onScaffold(bot) && (z > 1 || !atStrip)) {
        const col = freeColumn(strip, bot);
        if (!col) { foreman.say(`${bot.name}: no free column in front of strip ${strip.k}`); return; }
        const r = reach(bot.cell());
        if (!r.dist.has(cellOf(col))) { foreman.say(`${bot.name}: cannot walk to ${col.x}:${col.y}`); await sleep(2000); if (tries > 5) return; continue; }
        foreman.status.set(bot, `walking to its pillar at ${col.x}:${col.y}`);
        if (!(await bot.go(pathTo(r, col)))) continue;
        bot.pillar = `${col.x}:${col.y}`;
      }
      if (bot.cell().z < z - 1) {
        foreman.status.set(bot, `climbing to row ${z}`);
        if ((bot.inventory.dirt ?? 0) < z - 1 - bot.cell().z) { foreman.say(`${bot.name} is out of dirt for its pillar`); return; }
        if (!(await pillarUp(bot, z - 1))) { if (tries > 8) { foreman.say(`${bot.name} cannot raise its pillar past ${bot.cell().z}`); return; } await sleep(300); continue; }
      }
      break;
    }
    if (bot.dead) { while (bot.dead) await sleep(500); return; }
    foreman.status.set(bot, `row ${z}`);
    for (const b of row) {
      if (bot.dead) break;
      if (!(await placeOne(bot, b)) && !bot.dead) { foreman.blocked.add(cellOf(b)); foreman.say(`${bot.name}: could not place ${cellOf(b)} ${b.kind}, setting it aside`); }
    }
  }
  if (bot.dead) { while (bot.dead) await sleep(500); return; }
  strip.row = H;
  foreman.status.set(bot, "strip done, coming down");
}

// Down the pillar, taking old scaffolding within reach on the way.
async function comeDown(bot) {
  for (;;) {
    await bot.settle();
    const c = bot.cell();
    const near = [...scaffold].map(k => { const [x, y, z] = k.split(":").map(Number); return { x, y, z }; })
      .filter(o => !(o.x === c.x && o.y === c.y) && o.z >= c.z - 3 && !bots.some(b => { const bc = b.cell(); return bc.x === o.x && bc.y === o.y; }))
      .sort((a, b) => b.z - a.z);
    for (const o of near) {
      if (solid(o.x, o.y, o.z + 1)) continue;                     // from the top down
      if (!digFrom({ x: c.x, y: c.y, z: c.z }, o)) continue;
      const eye = bot.eye();
      let hit = null;
      for (const n of DIRS) for (const p of facePoints(o, n)) { const h = rayHit(eye, p); if (h && h.x === o.x && h.y === o.y && h.z === o.z) { hit = h; break; } }
      if (!hit) continue;
      bot.look(hit);
      bot.send({ op: "dig", state: "start", x: o.x, y: o.y, z: o.z });
      const until = Date.now() + (bot.breakTicks.dirt ?? 15) * S.TICK_MS + 2000;
      while (solid(o.x, o.y, o.z) && Date.now() < until) await sleep(40);
      if (!solid(o.x, o.y, o.z)) { scaffold.delete(key(o.x, o.y, o.z)); bot.dug++; }
      else bot.send({ op: "dig", state: "stop", x: o.x, y: o.y, z: o.z });
      await sleep(S.DIG_COOLDOWN_TICKS * S.TICK_MS);
    }
    if (!onScaffold(bot)) return;
    const before = scaffold.size;
    const c2 = bot.cell();
    // One block of its own pillar.
    const below = { x: c2.x, y: c2.y, z: c2.z - 1 };
    const hit = rayHit(bot.eye(), { x: below.x + 0.5, y: below.y + 0.5, z: below.z + 1 });
    if (!hit || hit.x !== below.x || hit.y !== below.y || hit.z !== below.z) return;
    bot.look(hit);
    bot.send({ op: "dig", state: "start", ...below });
    const until = Date.now() + (bot.breakTicks.dirt ?? 15) * S.TICK_MS + 2000;
    while (solid(below.x, below.y, below.z) && Date.now() < until) await sleep(40);
    if (solid(below.x, below.y, below.z)) { bot.send({ op: "dig", state: "stop", ...below }); return; }
    scaffold.delete(key(below.x, below.y, below.z));
    bot.dug++;
    await sleep(S.DIG_COOLDOWN_TICKS * S.TICK_MS);
    void before;
  }
}

async function clearColumn(bot, col) {
  const top = Math.max(...[...scaffold].map(k => k.split(":").map(Number)).filter(([x, y]) => x === col.x && y === col.y).map(([, , z]) => z));
  let spot = null;
  for (const [dx, dy] of [[0, -1], [0, 1], [-1, 0]]) {
    const x = col.x + dx, y = col.y + dy;
    let free = inRegion(x, y) && x < WALL_X && standable(x, y, 0);
    for (let z = 0; z <= top + 2 && free; z++) if (solid(x, y, z)) free = false;
    if (free) { spot = { x, y, z: 0 }; break; }
  }
  if (!spot) { foreman.say(`no room beside the scaffolding at ${col.x}:${col.y}`); return; }
  foreman.say(`${bot.name}: climbing beside the scaffolding at ${col.x}:${col.y} (top ${top}) to take it down`);
  foreman.status.set(bot, `taking down the scaffolding at ${col.x}:${col.y}`);
  await bot.settle();
  const r = reach(bot.cell());
  if (!r.dist.has(cellOf(spot)) || !(await bot.go(pathTo(r, spot)))) { foreman.say(`${bot.name} cannot get beside it`); return; }
  if (!(await pillarUp(bot, top + 1))) { foreman.say(`${bot.name} could not climb beside it`); }
  await comeDown(bot);
}

async function worker(bot, strip) {
  // A worker that dies (a bomb) comes back and carries on from a new pillar.
  for (let life = 0; life < 4 && strip.row !== H; life++) {
    await paintStrip(bot, strip);
    if (strip.row !== H) await sleep(3000);
  }
  foreman.status.set(bot, "coming down");
  await comeDown(bot);
  bot.pillar = null;
  foreman.status.set(bot, "done");
}
const origDeath = Bot.prototype.onFrame;
Bot.prototype.onFrame = function (f, resolve) {
  if (f.type === "death" && f.player === this.player?.player_id && bots.includes(this)) {
    this.pillar = null;
    foreman.say(`${this.name} was killed${f.by ? ` by ${f.by}` : ""}; it will come back and build a new pillar`);
  }
  return origDeath.call(this, f, resolve);
};

const total = plan.length;
const reportStrips = () => foreman.say(`${plan.filter(painted).length}/${total} painted${foreman.blocked.size ? `, set aside ${foreman.blocked.size}` : ""} — ` +
  STRIPS.map(s => `${bots[s.i].name}: ${foreman.status.get(bots[s.i]) ?? "?"}`).join("; "));
const stripReport = setInterval(reportStrips, 30000);
const stripWatch = setInterval(() => {
  for (const b of bots) if (Date.now() - (b.lastProgress ?? 0) > 45000 && foreman.status.get(b) !== "done" && !String(foreman.status.get(b)).startsWith("coming")) {
    foreman.say(`${b.name} has placed nothing for 45 s (${foreman.status.get(b)})`);
    b.lastProgress = Date.now();
  }
}, 5000);
const walkAbout = setInterval(() => foreman.walkAbout(bots).catch(() => {}), 8000);
if (true) {   // craters first, and any scaffolding left standing
  // Craters in front of the wall and under it are filled first, so there is ground to stand and build on.
  const holes = [];
  for (let z = -3; z <= -1; z++) for (let u = 0; u < W; u++) for (const d of [0, 1, 2, 3]) {
    const c = wallCell(u, z, d);
    if (!solid(c.x, c.y, z)) holes.push({ ...c, kind: z === -1 ? "grass" : "dirt" });
  }
  if (holes.length) { log("craters to fill:", holes.map(cellOf).join(" ")); await crew(bots, holes, "place", "craters filled"); }
  const cols = [...new Set([...scaffold].map(k => k.split(":").slice(0, 2).join(":")))].map(k => { const [x, y] = k.split(":").map(Number); return { x, y }; });
  await Promise.all(cols.map((c, i) => i < bots.length ? clearColumn(bots[i], c) : null));
}
await Promise.all(bots.map((b, k) => worker(b, STRIPS[k])));
clearInterval(stripReport); clearInterval(stripWatch); clearInterval(walkAbout);
reportStrips();
let left = plan.filter(b => !painted(b));
log(left.length ? `unfinished: ${left.map(b => `${cellOf(b)}=${b.kind}`).join(" ")}` : "the picture is finished!");
// Whatever scaffolding is still standing comes down, with scaffolding of its own if need be.
if (scaffold.size) {
  const rest = [...scaffold].map(k => { const [x, y, z] = k.split(":").map(Number); return { x, y, z }; });
  log("scaffolding left:", rest.length);
  await crew(bots, rest, "dig", "scaffolding down");
}
for (const b of bots) log(b.name, "placed", b.placed, "dug", b.dug);

// Everyone steps back to the west to look at it.
await Promise.all([...bots, foreman.bot].map(async (b, i) => {
  await b.settle();
  const r = reach(b.cell());
  const want = RY + 3 + i * 2;
  const spot = r.cells.filter(c => c.z === 0 && c.x === R + 6).sort((a, c) => Math.abs(a.y - want) - Math.abs(c.y - want))[0];
  if (spot) await b.go(pathTo(r, spot));
  b.look({ yaw: Math.atan2(-(WALL_X - b.body.x), 0), pitch: -0.9 });
}));
const STAY = Number(process.env.STAY ?? 60);
log(`staying ${STAY}s`);
await sleep(STAY * 1000);
EVERYONE.forEach(b => b.socket.close());
process.exit(0);
