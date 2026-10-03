// The world as a guard knows it, in the server's coordinates (x, y on the ground, z up): the generated
// terrain from the welcome frame plus every block that changed since.

import { buildTreeMap } from "./trees.mjs";

export const key = (x, y, z) => `${x}:${y}:${z}`;

export class World {
  constructor() {
    this.width = 72; this.depth = 48; this.minZ = -4; this.maxZ = 64; this.regionSize = 24;
    this.layers = new Map();
    this.trees = new Map();
    this.blocks = new Map();
    this.overrides = new Map();
    this.ready = false;
  }

  configure(welcome) {
    this.width = welcome.width; this.depth = welcome.depth; this.minZ = welcome.minZ; this.maxZ = welcome.maxZ;
    this.regionSize = welcome.regionSize ?? 24;
    this.layers = new Map(welcome.layers.map(l => [l.z, l.kind]));
    this.trees = buildTreeMap(welcome.trees ?? []);
    this.blocks = new Map(welcome.blocks.map(b => [b.kind, b]));
    this.overrides = new Map(welcome.world.map(c => [key(c.x, c.y, c.z), c]));
    this.ready = true;
  }

  inside(x, y, z) { return x >= 0 && x < this.width && y >= 0 && y < this.depth && z >= this.minZ && z < this.maxZ; }

  generated(x, y, z) {
    if (!this.inside(x, y, z)) return "air";
    if (z >= 0) return this.trees.get(key(x, y, z)) ?? "air";
    return this.layers.get(z) ?? "air";
  }

  /** The record that last changed the block, or null where the terrain is as generated. */
  cube(x, y, z) { return this.overrides.get(key(x, y, z)) ?? null; }

  kindAt(x, y, z) { return this.cube(x, y, z)?.kind ?? this.generated(x, y, z); }

  isSolid(x, y, z) {
    if (!this.inside(x, y, z)) return z < this.minZ;
    return this.kindAt(x, y, z) !== "air";
  }

  breakTicks(kind) { return this.blocks.get(kind)?.breakTicks ?? -1; }

  /** A change the server sent: "upsert" with the new block, or "delete" back to the terrain. */
  apply(op, cube) {
    const k = key(cube.x, cube.y, cube.z);
    if (op === "delete") this.overrides.delete(k); else this.overrides.set(k, cube);
  }

  regionOf(x, y) { return Math.floor(y / this.regionSize) * Math.floor(this.width / this.regionSize) + Math.floor(x / this.regionSize); }
}
