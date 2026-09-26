// The world as the builders know it, in the server's coordinates (x, y on the ground, z up): the generated
// terrain from the welcome frame, every block that changed since, and the blocks a builder has just placed and is
// waiting to hear back about (drawn at once, as the browser client draws its own placement).
// Also the hands' geometry: the client's own ray (web/voxels.js raycastBlocks), and points on a block's faces.

import * as S from "../web/spec.js";

export const key = (x, y, z) => `${x}:${y}:${z}`;
export const cellOf = c => key(c.x, c.y, c.z);
export const DIRS = [[0, 0, -1], [1, 0, 0], [-1, 0, 0], [0, 1, 0], [0, -1, 0], [0, 0, 1]];

// The oaks as the servers generate them (Spec.BuildTrees, web/voxels.js buildTreeMap).
export function buildTreeMap(trees) {
  const map = new Map();
  for (const { x: tx, y: ty } of trees) {
    for (let dz = 0; dz < 5; dz++) map.set(key(tx, ty, dz), "wood");
    for (let dx = -2; dx <= 2; dx++) for (let dy = -2; dy <= 2; dy++) {
      const corner = Math.abs(dx) === 2 && Math.abs(dy) === 2, trunk = dx === 0 && dy === 0;
      const add = z => { if (!map.has(key(tx + dx, ty + dy, z))) map.set(key(tx + dx, ty + dy, z), "leaves"); };
      for (let dz = 3; dz <= 4; dz++) if (!corner && !trunk) add(dz);
      if (Math.abs(dx) <= 1 && Math.abs(dy) <= 1) add(5);
      if (Math.abs(dx) + Math.abs(dy) <= 1) add(6);
    }
  }
  return map;
}

export class World {
  constructor() {
    this.width = 72; this.depth = 48; this.minZ = -4; this.maxZ = 64; this.regionSize = 24;
    this.layers = new Map(); this.trees = new Map(); this.blocks = new Map();
    this.overrides = new Map();       // key -> kind, as the servers last said
    this.unconfirmed = new Map();     // key -> { kind, at }: placed by a builder, not yet heard back
    this.hooks = new Set();           // called with a key whenever the servers change a block
    this.ready = false;
  }

  configure(welcome) {
    this.width = welcome.width; this.depth = welcome.depth; this.minZ = welcome.minZ; this.maxZ = welcome.maxZ;
    this.regionSize = welcome.regionSize ?? 24;
    this.layers = new Map(welcome.layers.map(l => [l.z, l.kind]));
    this.trees = buildTreeMap(welcome.trees ?? []);
    this.blocks = new Map(welcome.blocks.map(b => [b.kind, b]));
    for (const c of welcome.world) this.overrides.set(key(c.x, c.y, c.z), c.kind);
    this.ready = true;
  }

  inside(x, y, z) { return x >= 0 && x < this.width && y >= 0 && y < this.depth && z >= this.minZ && z < this.maxZ; }

  generated(x, y, z) {
    if (!this.inside(x, y, z)) return "air";
    if (z >= 0) return this.trees.get(key(x, y, z)) ?? "air";
    return this.layers.get(z) ?? "air";
  }

  /** What the servers say is there. */
  onServer(x, y, z) {
    if (!this.inside(x, y, z)) return "air";
    return this.overrides.get(key(x, y, z)) ?? this.generated(x, y, z);
  }

  /** What a builder sees: the servers' word, and its own blocks still on their way. */
  kindAt(x, y, z) { return this.unconfirmed.get(key(x, y, z))?.kind ?? this.onServer(x, y, z); }

  solid(x, y, z) { return this.inside(x, y, z) ? this.kindAt(x, y, z) !== "air" : z < this.minZ; }

  breakTicks(kind) { return this.blocks.get(kind)?.breakTicks ?? 200; }

  apply(op, cube) {
    const k = key(cube.x, cube.y, cube.z);
    if (op === "delete") this.overrides.delete(k); else this.overrides.set(k, cube.kind);
    this.unconfirmed.delete(k);
    for (const h of this.hooks) h(k);
  }

  /** A builder's own block, drawn until the server answers (or forgotten after 3 s). */
  expect(k, kind, at) { this.unconfirmed.set(k, { kind, at }); }
  forget(k) { this.unconfirmed.delete(k); }
  sweep(now) { for (const [k, u] of this.unconfirmed) if (now - u.at > 3000) this.unconfirmed.delete(k); }

  regionOf(x, y) { return Math.floor(y / this.regionSize) * Math.floor(this.width / this.regionSize) + Math.floor(x / this.regionSize); }

  // What the body collides with, in the client's coordinates (x, z on the ground, y up): blocks, and the world's
  // border and floor as walls (VoxelWorld.isSolidForPhysics).
  physicsSolid = (x, y, z) => (x < 0 || x >= this.width || z < 0 || z >= this.depth || y < this.minZ) ? true : this.solid(x, z, y);
}

// raycastBlocks from web/voxels.js, as it is (voxels.js itself pulls in three.js), in the client's coordinates.
function raycastBlocks(isSolid, origin, direction, reach) {
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
    if (isSolid(x, y, z) && face) return { x, y, z, nx: face[0], ny: face[1], nz: face[2], distance: travelled };
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

/** The ray from an eye to a point, both in server coordinates: the block the client would hit, and the look. */
export function rayHit(world, eye, point) {
  const d = { x: point.x - eye.x, y: point.z - eye.z, z: point.y - eye.y };
  const len = Math.hypot(d.x, d.y, d.z);
  if (len < 1e-6) return null;
  const hit = raycastBlocks((x, y, z) => world.solid(x, z, y), { x: eye.x, y: eye.z, z: eye.y }, { x: d.x / len, y: d.y / len, z: d.z / len }, S.BLOCK_REACH);
  return hit && { x: hit.x, y: hit.z, z: hit.y, nx: hit.nx, ny: hit.nz, nz: hit.ny, yaw: Math.atan2(-d.x, d.z), pitch: -Math.asin(d.y / len) };
}

const face = (b, n) => ({ x: b.x + 0.5 + n[0] * 0.5, y: b.y + 0.5 + n[1] * 0.5, z: b.z + 0.5 + n[2] * 0.5 });
const axesOf = n => [[1, 0, 0], [0, 1, 0], [0, 0, 1]].filter(a => a[0] * n[0] + a[1] * n[1] + a[2] * n[2] === 0);

/** Five points on the face of b towards n: the centre and four inset. Cheap, for planning. */
export function facePoints(b, n) {
  const c = face(b, n), pts = [c];
  for (const s of [0.35, -0.35]) for (const a of axesOf(n)) pts.push({ x: c.x + a[0] * s, y: c.y + a[1] * s, z: c.z + a[2] * s });
  return pts;
}

/** A 5 × 5 grid on the face: for aiming from where a body really stands, a little off its cell's middle. */
export function faceGrid(b, n) {
  const c = face(b, n), [u, v] = axesOf(n), pts = [];
  for (const a of [0, 0.2, -0.2, 0.4, -0.4]) for (const e of [0, 0.2, -0.2, 0.4, -0.4])
    pts.push({ x: c.x + u[0] * a + v[0] * e, y: c.y + u[1] * a + v[1] * e, z: c.z + u[2] * a + v[2] * e });
  return pts;
}

/** How an eye would place block b: a ray that hits a solid neighbour of b, on its face towards b. */
export function aimPlace(world, eye, b, fine = false) {
  const pts = fine ? faceGrid : facePoints;
  for (const [dx, dy, dz] of DIRS) {
    const a = { x: b.x + dx, y: b.y + dy, z: b.z + dz };
    if (!world.solid(a.x, a.y, a.z)) continue;
    const n = [-dx, -dy, -dz];
    for (const p of pts(a, n)) {
      const h = rayHit(world, eye, p);
      if (h && h.x === a.x && h.y === a.y && h.z === a.z && h.nx === n[0] && h.ny === n[1] && h.nz === n[2]) return h;
    }
  }
  return null;
}

/** How an eye would break block b: a ray that hits b itself first. */
export function aimDig(world, eye, b, fine = false) {
  const pts = fine ? faceGrid : facePoints;
  for (const n of DIRS) for (const p of pts(b, n)) {
    const h = rayHit(world, eye, p);
    if (h && h.x === b.x && h.y === b.y && h.z === b.z) return h;
  }
  return null;
}

export const eyeOfCell = s => ({ x: s.x + 0.5, y: s.y + 0.5, z: s.z + S.EYE_HEIGHT });

/** As World.Intersects on the server: a body (client coords) overlaps block b. */
export const overlaps = (body, b) => body.x - 0.3 < b.x + 1 && body.x + 0.3 > b.x && body.z - 0.3 < b.y + 1 && body.z + 0.3 > b.y && body.y < b.z + 1 && body.y + S.HEIGHT > b.z;
