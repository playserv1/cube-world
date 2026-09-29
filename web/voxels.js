// The client's copy of the world (x and z on the ground, y up) and its rendering: chunks of 16 × 16
// columns meshed with hidden faces culled, faces lit as Minecraft lights them (top 1, sides 0.8 and
// 0.6, bottom 0.5), glass drawn as a cutout with faces between two glass blocks dropped.

import * as THREE from "three";
import { FACES, grassFaces } from "./textures.js";

const CHUNK = 16;
const key = (x, y, z) => `${x},${y},${z}`;

// Where the oaks stand, in the server's ground coordinates (x, y): four per region, clear of the spawn.
// Mirrors Spec.Trees on the server; the server also sends the list in its welcome frame.
export const TREES = [0, 1, 2].flatMap(r => [[r * 24 + 4, 5], [r * 24 + 18, 4], [r * 24 + 6, 18], [r * 24 + 19, 17]].map(([x, y]) => ({ x, y })));

// An oak in client coordinates: five logs, two 5 × 5 leaf layers without corners around the top two logs,
// a 3 × 3 layer above the trunk and a cross on top. Mirrors Spec.BuildTrees on the server.
export function buildTreeMap(trees) {
  const map = new Map();
  for (const { x: tx, y: ty } of trees) {
    for (let dy = 0; dy < 5; dy++) map.set(key(tx, dy, ty), "wood");
    for (let dx = -2; dx <= 2; dx++)
      for (let dz = -2; dz <= 2; dz++) {
        const corner = Math.abs(dx) === 2 && Math.abs(dz) === 2, trunk = dx === 0 && dz === 0;
        for (let dy = 3; dy <= 4; dy++) if (!corner && !trunk && !map.has(key(tx + dx, dy, ty + dz))) map.set(key(tx + dx, dy, ty + dz), "leaves");
        if (Math.abs(dx) <= 1 && Math.abs(dz) <= 1 && !map.has(key(tx + dx, 5, ty + dz))) map.set(key(tx + dx, 5, ty + dz), "leaves");
        if (Math.abs(dx) + Math.abs(dz) <= 1 && !map.has(key(tx + dx, 6, ty + dz))) map.set(key(tx + dx, 6, ty + dz), "leaves");
      }
  }
  return map;
}

// Each face: normal, the four corners as bottom-left, bottom-right, top-right, top-left seen from outside,
// its tile (top, side, bottom) and its light.
const SIDES = [
  { n: [1, 0, 0], c: [[1, 0, 1], [1, 0, 0], [1, 1, 0], [1, 1, 1]], tile: "side", light: 0.6 },
  { n: [-1, 0, 0], c: [[0, 0, 0], [0, 0, 1], [0, 1, 1], [0, 1, 0]], tile: "side", light: 0.6 },
  { n: [0, 1, 0], c: [[0, 1, 1], [1, 1, 1], [1, 1, 0], [0, 1, 0]], tile: "top", light: 1.0 },
  { n: [0, -1, 0], c: [[0, 0, 0], [1, 0, 0], [1, 0, 1], [0, 0, 1]], tile: "bottom", light: 0.5 },
  { n: [0, 0, 1], c: [[0, 0, 1], [1, 0, 1], [1, 1, 1], [0, 1, 1]], tile: "side", light: 0.8 },
  { n: [0, 0, -1], c: [[1, 0, 0], [0, 0, 0], [0, 1, 0], [1, 1, 0]], tile: "side", light: 0.8 },
];

export class VoxelWorld {
  constructor() {
    this.width = 72; this.depth = 24; this.minY = -4; this.maxY = 64;
    this.layers = new Map();
    this.trees = new Map();   // the live servers send the oaks as records; only the offline world generates them
    this.blocks = new Map();
    this.overrides = new Map();
    this.hidden = new Set();
    this.regionSize = 24;
    this.regionColors = ["red", "blue", "green"];
  }

  configure({ width, depth, minY, maxY, layers, blocks, trees, regionSize, regionColors }) {
    this.width = width; this.depth = depth; this.minY = minY; this.maxY = maxY;
    this.layers = new Map(layers.map(l => [l.z, l.kind]));
    this.trees = buildTreeMap(trees ?? []);
    this.blocks = new Map(blocks.map(b => [b.kind, b]));
    this.regionSize = regionSize ?? this.regionSize;
    this.regionColors = regionColors ?? this.regionColors;
    this.overrides.clear();
    this.hidden.clear();
  }

  block(kind) { return this.blocks.get(kind) ?? this.blocks.get("air") ?? { kind: "air", Transparent: true }; }

  inside(x, y, z) { return x >= 0 && x < this.width && z >= 0 && z < this.depth && y >= this.minY && y < this.maxY; }

  generated(x, y, z) {
    if (!this.inside(x, y, z)) return "air";
    if (y >= 0) return this.trees.get(key(x, y, z)) ?? "air";
    return this.layers.get(y) ?? "air";
  }

  regionColor(x) { return this.regionColors[Math.floor(x / this.regionSize)] ?? "green"; }

  kindAt(x, y, z) {
    const k = key(x, y, z);
    if (this.hidden.has(k)) return "air";
    return this.overrides.get(k) ?? this.generated(x, y, z);
  }

  isSolid(x, y, z) { return this.inside(x, y, z) && this.kindAt(x, y, z) !== "air"; }

  // What the player's body collides with: blocks, and the world border and floor as walls.
  isSolidForPhysics = (x, y, z) => {
    if (x < 0 || x >= this.width || z < 0 || z >= this.depth || y < this.minY) return true;
    return this.isSolid(x, y, z);
  };

  // Sets an override (null restores the generated block). Returns the chunk ids to rebuild.
  set(x, y, z, kind) {
    const k = key(x, y, z);
    if (kind === null) this.overrides.delete(k); else this.overrides.set(k, kind);
    return this.chunksAround(x, z);
  }

  hide(x, y, z, on) {
    const k = key(x, y, z);
    if (on) this.hidden.add(k); else this.hidden.delete(k);
    return this.chunksAround(x, z);
  }

  chunksAround(x, z) {
    const ids = new Set();
    for (const [dx, dz] of [[0, 0], [1, 0], [-1, 0], [0, 1], [0, -1]]) {
      const cx = Math.floor((x + dx) / CHUNK), cz = Math.floor((z + dz) / CHUNK);
      if (cx >= 0 && cz >= 0 && cx * CHUNK < this.width && cz * CHUNK < this.depth) ids.add(`${cx},${cz}`);
    }
    return ids;
  }

  allChunks() {
    const ids = [];
    for (let cx = 0; cx * CHUNK < this.width; cx++) for (let cz = 0; cz * CHUNK < this.depth; cz++) ids.push(`${cx},${cz}`);
    return ids;
  }
}

function faceVisible(world, kind, transparent, nx, ny, nz) {
  if (ny < world.minY) return false;
  const neighbour = world.inside(nx, ny, nz) ? world.kindAt(nx, ny, nz) : "air";
  if (neighbour === "air") return true;
  const other = world.block(neighbour);
  if (!other.Transparent) return false;
  return transparent ? neighbour !== kind : true;
}

// Builds the two meshes (opaque, cutout) of one chunk.
export function meshChunk(world, atlas, materials, id) {
  const [cx, cz] = id.split(",").map(Number);
  const parts = { opaque: { pos: [], uv: [], col: [], idx: [] }, cutout: { pos: [], uv: [], col: [], idx: [] } };
  for (let x = cx * CHUNK; x < Math.min(world.width, (cx + 1) * CHUNK); x++)
    for (let z = cz * CHUNK; z < Math.min(world.depth, (cz + 1) * CHUNK); z++)
      for (let y = world.minY; y < world.maxY; y++) {
        const kind = world.kindAt(x, y, z);
        if (kind === "air") continue;
        const block = world.block(kind);
        const faces = kind === "grass" ? grassFaces(world.regionColor(x)) : FACES[kind] ?? FACES.stone;
        const part = block.Transparent ? parts.cutout : parts.opaque;
        for (const side of SIDES) {
          if (!faceVisible(world, kind, block.Transparent, x + side.n[0], y + side.n[1], z + side.n[2])) continue;
          const { u0, u1, v0, v1 } = atlas.uv(faces[side.tile]);
          const base = part.pos.length / 3;
          for (const [dx, dy, dz] of side.c) part.pos.push(x + dx, y + dy, z + dz);
          part.uv.push(u0, v0, u1, v0, u1, v1, u0, v1);
          for (let i = 0; i < 4; i++) part.col.push(side.light, side.light, side.light);
          part.idx.push(base, base + 1, base + 2, base, base + 2, base + 3);
        }
      }

  const group = new THREE.Group();
  for (const [name, part] of Object.entries(parts)) {
    if (!part.idx.length) continue;
    const geometry = new THREE.BufferGeometry();
    geometry.setAttribute("position", new THREE.Float32BufferAttribute(part.pos, 3));
    geometry.setAttribute("uv", new THREE.Float32BufferAttribute(part.uv, 2));
    geometry.setAttribute("color", new THREE.Float32BufferAttribute(part.col, 3));
    geometry.setIndex(part.idx);
    geometry.computeBoundingSphere();
    const mesh = new THREE.Mesh(geometry, materials[name]);
    mesh.userData.chunk = id;
    group.add(mesh);
  }
  return group;
}

export function chunkMaterials(atlas) {
  return {
    opaque: new THREE.MeshBasicMaterial({ map: atlas.texture, vertexColors: true }),
    cutout: new THREE.MeshBasicMaterial({ map: atlas.texture, vertexColors: true, alphaTest: 0.5, side: THREE.DoubleSide }),
  };
}

// One block as a mesh: the falling sand, or anything else drawn on its own.
export function blockMesh(atlas, materials, kind) {
  const faces = FACES[kind] ?? FACES.stone;
  const pos = [], uv = [], col = [], idx = [];
  for (const side of SIDES) {
    const { u0, u1, v0, v1 } = atlas.uv(faces[side.tile]);
    const base = pos.length / 3;
    for (const [dx, dy, dz] of side.c) pos.push(dx - 0.5, dy - 0.5, dz - 0.5);
    uv.push(u0, v0, u1, v0, u1, v1, u0, v1);
    for (let i = 0; i < 4; i++) col.push(side.light, side.light, side.light);
    idx.push(base, base + 1, base + 2, base, base + 2, base + 3);
  }
  const geometry = new THREE.BufferGeometry();
  geometry.setAttribute("position", new THREE.Float32BufferAttribute(pos, 3));
  geometry.setAttribute("uv", new THREE.Float32BufferAttribute(uv, 2));
  geometry.setAttribute("color", new THREE.Float32BufferAttribute(col, 3));
  geometry.setIndex(idx);
  return new THREE.Mesh(geometry, kind === "glass" ? materials.cutout : materials.opaque);
}

// The crack overlay drawn over a block being dug: a slightly larger cube showing one crack stage.
export function crackMesh(atlas) {
  const geometry = new THREE.BoxGeometry(1.002, 1.002, 1.002);
  const material = new THREE.MeshBasicMaterial({ map: atlas.texture, transparent: true, depthWrite: false, polygonOffset: true, polygonOffsetFactor: -1 });
  const mesh = new THREE.Mesh(geometry, material);
  mesh.userData.setStage = stage => {
    const { u0, u1, v0, v1 } = atlas.uv(`crack${Math.max(0, Math.min(9, stage))}`);
    const uv = geometry.attributes.uv;
    for (let face = 0; face < 6; face++) {
      const values = [u0, v1, u1, v1, u0, v0, u1, v0];
      for (let i = 0; i < 4; i++) uv.setXY(face * 4 + i, values[i * 2], values[i * 2 + 1]);
    }
    uv.needsUpdate = true;
  };
  mesh.userData.setStage(0);
  return mesh;
}

// Walks the ray block by block (Amanatides & Woo) and returns the first solid block and the face it entered.
export function raycastBlocks(world, origin, direction, reach) {
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

// The nearest player hitbox the ray passes through, within reach.
export function raycastPlayers(origin, direction, reach, players) {
  let best = null;
  for (const p of players) {
    const min = [p.x - 0.3, p.y, p.z - 0.3], max = [p.x + 0.3, p.y + p.height, p.z + 0.3];
    let tmin = 0, tmax = reach;
    const o = [origin.x, origin.y, origin.z], d = [direction.x, direction.y, direction.z];
    let ok = true;
    for (let a = 0; a < 3 && ok; a++) {
      if (Math.abs(d[a]) < 1e-9) { if (o[a] < min[a] || o[a] > max[a]) ok = false; continue; }
      let t1 = (min[a] - o[a]) / d[a], t2 = (max[a] - o[a]) / d[a];
      if (t1 > t2) [t1, t2] = [t2, t1];
      tmin = Math.max(tmin, t1); tmax = Math.min(tmax, t2);
      if (tmin > tmax) ok = false;
    }
    if (ok && (!best || tmin < best.distance)) best = { player: p, distance: tmin };
  }
  return best;
}
