// The room the guards keep: its box, the part each guard answers for, the posts by the entrance, and the
// blueprint, the kind every cell of the box should hold. room.json keeps the blueprint as one picture per level,
// a row per y and a character per x, so it can be read and edited by hand.

export const LEGEND = { ".": "air", G: "gold", g: "grass", d: "dirt", W: "wood", l: "leaves", "#": "bedrock", s: "stone", b: "brick", o: "glass", S: "sand" };
const CHAR = Object.fromEntries(Object.entries(LEGEND).map(([c, k]) => [k, c]));

export class Room {
  constructor(spec) {
    this.spec = spec;
    const { box } = spec;
    this.box = box;
    this.cells = new Map();
    for (const [z, rows] of Object.entries(spec.blueprint ?? {}))
      rows.forEach((row, i) => [...row].forEach((c, j) => this.cells.set(`${box.x0 + j}:${box.y0 + i}:${z}`, LEGEND[c] ?? "air")));
  }

  /** Records the world as it stands now as the blueprint. */
  static snapshot(spec, world) {
    const { box } = spec, blueprint = {};
    for (let z = box.z1; z >= box.z0; z--) {
      blueprint[z] = [];
      for (let y = box.y0; y <= box.y1; y++) {
        let row = "";
        for (let x = box.x0; x <= box.x1; x++) row += CHAR[world.kindAt(x, y, z)] ?? "?";
        blueprint[z].push(row);
      }
    }
    return new Room({ ...spec, blueprint });
  }

  toJSON() {
    const { box } = this, blueprint = {};
    for (let z = box.z1; z >= box.z0; z--) {
      blueprint[z] = [];
      for (let y = box.y0; y <= box.y1; y++) {
        let row = "";
        for (let x = box.x0; x <= box.x1; x++) row += CHAR[this.cells.get(`${x}:${y}:${z}`)] ?? "?";
        blueprint[z].push(row);
      }
    }
    return { ...this.spec, blueprint };
  }

  inBox(x, y, z) {
    const b = this.box;
    return x >= b.x0 && x <= b.x1 && y >= b.y0 && y <= b.y1 && z >= b.z0 && z <= b.z1;
  }

  /** The kind the blueprint wants at a cell, or undefined outside the box. */
  wanted(x, y, z) { return this.cells.get(`${x}:${y}:${z}`); }

  /** The owner rebuilt a cell: the blueprint takes the new kind. */
  adopt(x, y, z, kind) { if (this.inBox(x, y, z)) this.cells.set(`${x}:${y}:${z}`, kind); }

  /** Which guard answers for a cell: the roof and what stands on it, or the rooms under it. */
  partOf(z) { return z >= this.spec.surfaceZ ? "surface" : "vault"; }

  /** Every cell of one part that is not as the blueprint wants it; bedrock cannot be restored and is skipped. */
  damage(world, part) {
    const out = [];
    for (const [k, kind] of this.cells) {
      const [x, y, z] = k.split(":").map(Number);
      if (this.partOf(z) !== part) continue;
      const now = world.kindAt(x, y, z);
      if (now !== kind && kind !== "bedrock" && now !== "bedrock") out.push({ x, y, z, want: kind, now });
    }
    return out;
  }

  /** Whether a player (feet in server coordinates) is down in the room or its corridor. */
  isInside(p) {
    const b = this.box;
    return p.x >= b.x0 && p.x < b.x1 + 1 && p.y >= b.y0 && p.y < b.y1 + 1 && p.z < this.spec.surfaceZ + 0.5;
  }
}
