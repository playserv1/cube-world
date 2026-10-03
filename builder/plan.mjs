// The house: ten storeys over a whole region, 24 × 24. Local coordinates i (x) and j (y) from the region's
// south-west corner, 0 and 23 the outer walls. Each storey: three blocks of room and a slab over it; a corridor two
// wide down the middle (j 11 and 12) with four rooms on each side and a door into each; two staircases, one at
// each end of the corridor, up through a hole in every slab and out onto the roof, which has a glass railing.
//
// The house is cut into zones, one builder each: the eight rooms (a room's walls, its furniture, the slab over it)
// and the two halves of the corridor (the stairs, the slab over it). A room's builder works from inside its room
// and never walks through a door. It goes up a storey through a hatch: every room has a table in the middle; the
// slab over the table is left open; the builder stands on the table, puts two blocks of dirt under itself, climbs
// out through the hatch, takes the dirt away from above and closes the hatch. The rooms stand one over the other,
// so it is in its own room again, a storey up. The corridor's builders use the stairs.
//
// Every block carries its storey, its zone and a phase. A zone takes a storey up in phases, in an order that makes
// every block reachable when its turn comes:
//   0 junctions   the corners, and every block where two walls meet, first, while the walls beside them are
//                 still open (a block between finished walls on three sides can only be seen from outside);
//   1 walls       outer walls, corridor walls, the walls between rooms, the stairs;
//   2 furniture   once the walls stand (a cabinet against a wall would hide the wall behind it), and the table;
//   3 slab        corners, then the edge over the outer walls, then the rest, from scaffolding;
//   4 hatch       the slab over the table, from above, once its builder has climbed out;
//   5 railing     round the roof.

export const STOREYS = 10, H = 4;                 // storey s: walls at z 4s..4s+2, its slab at 4s+3
export const ROOF = STOREYS * H - 1;              // z 39
export const PHASES = ["junctions", "walls", "furniture", "slab", "hatch", "railing"];

const ROOMS = [[1, 5], [7, 11], [13, 16], [18, 22]];       // the four rooms on each side, by column
const PARTITIONS = [6, 12, 17];
const SOUTH_DOORS = [3, 9, 14, 18];                        // into the rooms south of the corridor (wall j 10)
const NORTH_DOORS = [5, 9, 14, 20];                        // north of it (wall j 13); clear of the west stairs' wall
// Each flight: three steps along a corridor wall, the wall holding them up, a landing on the slab above.
export const FLIGHTS = [
  { name: "east", steps: [[19, 11], [20, 11], [21, 11]], landing: [22, 11] },   // against the wall at j 10
  { name: "west", steps: [[2, 12], [3, 12], [4, 12]], landing: [5, 12] },       // against the wall at j 13
];

const SLAB = ["wood", "stone", "grass", "dirt", "wood", "stone", "grass", "dirt", "wood", "stone"];
const OUTER = s => (s < 5 ? "brick" : "gold");
const INNER = s => (s % 2 === 0 ? "glass" : "leaves");

// What goes in a room, by kind: [across, deep, up, block], across from the room's back corner, deep from its back wall.
const FURNITURE = {
  bedroom: [[0, 0, 0, "sand"], [0, 1, 0, "brick"], [0, 2, 0, "brick"], [1, 0, 0, "gold"], [3, 0, 0, "wood"], [3, 0, 1, "wood"]],
  kitchen: [[0, 0, 0, "stone"], [1, 0, 0, "stone"], [2, 0, 0, "gold"], [3, 0, 0, "stone"], [1, 3, 0, "wood"], [0, 3, 0, "stone"], [2, 3, 0, "stone"]],
  living: [[0, 0, 0, "wood"], [1, 0, 0, "wood"], [2, 0, 0, "wood"], [1, 2, 0, "glass"], [3, 0, 0, "leaves"], [3, 4, 0, "gold"]],
  office: [[1, 1, 0, "stone"], [2, 1, 0, "stone"], [1, 1, 1, "gold"], [1, 2, 0, "wood"], [3, 0, 0, "wood"], [3, 0, 1, "wood"], [3, 1, 0, "wood"], [3, 1, 1, "wood"]],
  bathroom: [[0, 0, 0, "glass"], [1, 0, 0, "glass"], [0, 0, 1, "glass"], [3, 0, 0, "stone"], [3, 0, 1, "glass"]],
  dining: [[1, 2, 0, "wood"], [2, 2, 0, "wood"], [0, 2, 0, "stone"], [3, 2, 0, "stone"], [0, 0, 0, "leaves"]],
  library: [[0, 0, 0, "wood"], [1, 0, 0, "wood"], [2, 0, 0, "wood"], [3, 0, 0, "wood"], [0, 0, 1, "wood"], [1, 0, 1, "wood"], [2, 0, 1, "wood"], [3, 0, 1, "wood"], [1, 3, 0, "brick"]],
  nursery: [[0, 0, 0, "brick"], [0, 1, 0, "brick"], [2, 2, 0, "gold"], [3, 3, 0, "leaves"], [1, 4, 0, "glass"]],
};
const ROOM_KINDS = Object.keys(FURNITURE);

/** The rooms, as zones: name, columns, rows, and the table in the middle the builder climbs from. */
export const ROOM_ZONES = ROOMS.flatMap(([i0, i1], n) => [
  { name: `room S${n + 1}`, i0, i1, j0: 1, j1: 9, table: [Math.floor((i0 + i1) / 2), 7] },
  { name: `room N${n + 1}`, i0, i1, j0: 14, j1: 22, table: [Math.floor((i0 + i1) / 2), 16] },
]);
export const ZONES = [...ROOM_ZONES.map(z => z.name), "corridor W", "corridor E"];

function roomAt(i, j) { return ROOM_ZONES.find(z => i >= z.i0 && i <= z.i1 && j >= z.j0 && j <= z.j1)?.name ?? null; }

/** Whose a column is: a room's inside, the corridor, or a wall, which goes to a room beside it (it is seen from there). */
export function zoneOf(i, j) {
  const own = roomAt(i, j);
  if (own) return own;
  const corridor = i < 12 ? "corridor W" : "corridor E";
  // the corridor, and the stretches of its walls the stairs stand against (the rest of its walls go to the rooms)
  if (j >= 11 && j <= 12 && i >= 1 && i <= 22) return corridor;
  if ((j === 10 && i >= 18 && i <= 22) || (j === 13 && i >= 1 && i <= 5)) return corridor;
  const four = [[0, -1], [0, 1], [-1, 0], [1, 0]].map(([di, dj]) => roomAt(i + di, j + dj)).filter(Boolean);
  if (four.length) return four[0];
  if ([[0, -1], [0, 1], [-1, 0], [1, 0]].some(([di, dj]) => { const a = i + di, b = j + dj; return b >= 11 && b <= 12 && a >= 1 && a <= 22; })) return corridor;
  const eight = [[-1, -1], [1, -1], [-1, 1], [1, 1]].map(([di, dj]) => roomAt(i + di, j + dj)).filter(Boolean);
  return eight[0] ?? corridor;
}

/**
 * Every block of the house in a region whose south-west corner is (rx, ry), as
 * { x, y, z, kind, storey, phase, role, zone }. The ground under it is part of the plan too (storey -1).
 */
export function housePlan(rx, ry) {
  const blocks = new Map();
  const at = (i, j, z, kind, storey, phase, role) => blocks.set(`${rx + i}:${ry + j}:${z}`, { x: rx + i, y: ry + j, z, kind, storey, phase, role, zone: zoneOf(i, j) });
  const off = (i, j, z) => blocks.delete(`${rx + i}:${ry + j}:${z}`);
  const edge = (i, j) => i === 0 || i === 23 || j === 0 || j === 23;
  const corner = (i, j) => (i === 0 || i === 23) && (j === 0 || j === 23);

  for (let i = 0; i < 24; i++) for (let j = 0; j < 24; j++)
    for (const z of [-3, -2, -1]) at(i, j, z, z === -1 ? "grass" : "dirt", -1, 0, "ground");

  for (let s = 0; s < STOREYS; s++) {
    const base = s * H;
    for (let dz = 0; dz < 3; dz++) {
      const z = base + dz;
      for (let t = 0; t < 24; t++) for (const [i, j] of [[t, 0], [t, 23], [0, t], [23, t]]) {
        if (corner(i, j)) at(i, j, z, OUTER(s), s, 0, "corner");
        else at(i, j, z, dz === 1 && t % 4 === 2 ? "glass" : OUTER(s), s, 1, "facade");
      }
      for (let i = 1; i <= 22; i++) { at(i, 10, z, INNER(s), s, 1, "inner"); at(i, 13, z, INNER(s), s, 1, "inner"); }
      for (const i of PARTITIONS) for (let j = 1; j <= 22; j++) if (j <= 9 || j >= 14) at(i, j, z, INNER(s), s, 1, "inner");
    }
    for (const dz of [0, 1]) { for (const i of SOUTH_DOORS) off(i, 10, base + dz); for (const i of NORTH_DOORS) off(i, 13, base + dz); }
    for (let dz = 0; dz < 3; dz++) {
      const z = base + dz, wall = (i, j) => { const b = blocks.get(`${rx + i}:${ry + j}:${z}`); return b && (b.role === "facade" || b.role === "inner" || b.role === "corner"); };
      for (let i = 0; i < 24; i++) for (let j = 0; j < 24; j++) {
        const b = blocks.get(`${rx + i}:${ry + j}:${z}`);
        if (!b || (b.role !== "facade" && b.role !== "inner")) continue;
        if ((wall(i - 1, j) || wall(i + 1, j)) && (wall(i, j - 1) || wall(i, j + 1))) Object.assign(b, { phase: 0, role: "junction" });
      }
    }
    // Furniture: the south rooms' back wall is at j 1, the north rooms' at j 22; and the table in the middle.
    ROOMS.forEach(([i0, i1], n) => {
      for (const side of ["south", "north"]) {
        const kind = ROOM_KINDS[(s * 3 + n * 2 + (side === "north" ? 1 : 0)) % ROOM_KINDS.length];
        // In a corner room, nothing stands in the cell beside the house's corner: the corner is seen from there.
        const corner = (n === 0 || n === ROOMS.length - 1);
        for (const [a, d, u, k] of FURNITURE[kind]) {
          if (a > i1 - i0) continue;
          const i = side === "south" ? i0 + a : i1 - a;
          if (corner && d === 0 && (i === 1 || i === 22)) continue;
          at(i, side === "south" ? 1 + d : 22 - d, base + u, k, s, 2, "furniture");
        }
      }
    });
    for (const z of ROOM_ZONES) at(z.table[0], z.table[1], base, "wood", s, 2, "table");
    // The slab, open over both flights; the hatch over every table; the steps, each against its corridor wall.
    const slabKind = s === STOREYS - 1 ? "stone" : SLAB[s];
    for (let i = 0; i < 24; i++) for (let j = 0; j < 24; j++)
      at(i, j, base + 3, slabKind, s, 3, corner(i, j) ? "slab corner" : edge(i, j) ? "slab edge" : "slab");
    for (const z of ROOM_ZONES) at(z.table[0], z.table[1], base + 3, slabKind, s, 4, "hatch");
    for (const f of FLIGHTS) f.steps.forEach(([i, j], n) => { at(i, j, base + n, "wood", s, 1, "stairs"); off(i, j, base + 3); });
  }
  // The ground floor's front door, west, out to the next region; a railing round the roof.
  for (const j of [11, 12]) for (const z of [0, 1]) off(0, j, z);
  for (let t = 0; t < 24; t++) for (const [i, j] of [[t, 0], [t, 23], [0, t], [23, t]]) at(i, j, ROOF + 1, "glass", STOREYS - 1, 5, "railing");
  return [...blocks.values()];
}

/**
 * The order a zone's blocks go in: storey, then phase; in the slab, corners and edge before the rest. Within a
 * phase there is no order by height: a block can only go against one already there, so a wall goes up from the
 * floor anyway, and a builder finishes all it can reach from where it stands before it walks on.
 */
export function rank(b) {
  const sub = b.role === "slab corner" ? 0 : b.role === "slab edge" ? 1 : 2;
  return ((b.storey + 1) * 10 + b.phase) * 1000 + (b.phase === 3 ? sub * 100 : 0);
}

/** The table a room's builder climbs from, in world coordinates, for a block's zone. */
export function tableOf(zone, rx, ry) {
  const z = ROOM_ZONES.find(r => r.name === zone);
  return z ? { x: rx + z.table[0], y: ry + z.table[1] } : null;
}
