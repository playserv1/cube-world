// The house: STOREYS storeys on a SIZE × SIZE footprint in the middle of a 24 × 24 region, with MARGIN blocks of
// open ground all round it to the region's edge (5 storeys, 20 × 20, a margin of 2 by default). Local coordinates
// i (x) and j (y) from the region's south-west corner; the outer walls stand at LO and HI. Each storey: three blocks
// of room and a slab over it; a corridor two wide across the middle with four rooms on each side and a door into
// each; two staircases, one at each end of the corridor, up through a hole in every slab and out onto the roof,
// which has a glass railing. Everything below is worked out from those three numbers.
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
//   3 slab        corners, then the edge, then the rest (over an inner wall before both its neighbours), from scaffolding;
//   4 hatch       the slab over the table, from above, once its builder has climbed out;
//   5 railing     round the roof.

export const STOREYS = 5, SIZE = 20, MARGIN = 2;
export const H = 4;                               // storey s: walls at z 4s..4s+2, its slab at 4s+3
export const ROOF = STOREYS * H - 1;              // the roof slab's z
export const PHASES = ["junctions", "walls", "furniture", "slab", "hatch", "railing"];

const LO = MARGIN, HI = MARGIN + SIZE - 1;        // the outer walls
const C0 = LO + SIZE / 2 - 1, C1 = C0 + 1;        // the corridor's two rows
const W0 = C0 - 1, W1 = C1 + 1;                   // its walls
const MID = LO + SIZE / 2;                        // where its west half ends and its east half begins

// Four rooms a side between the outer walls, three walls between them; the last room takes what is left over.
const ROOMS = (() => {
  const free = (HI - 1) - (LO + 1) + 1 - 3, width = Math.ceil(free / 4), out = [];
  let i = LO + 1;
  for (let n = 0; n < 4; n++) { const w = n < 3 ? width : free - 3 * width; out.push([i, i + w - 1]); i += w + 1; }
  return out;
})();
const PARTITIONS = ROOMS.slice(0, 3).map(([, i1]) => i1 + 1);

// Each flight: three steps along a corridor wall, the wall holding the upper two up, a landing on the slab above.
export const FLIGHTS = [
  { name: "east", steps: [[HI - 4, C0], [HI - 3, C0], [HI - 2, C0]], landing: [HI - 1, C0], wall: W0 },
  { name: "west", steps: [[LO + 2, C1], [LO + 3, C1], [LO + 4, C1]], landing: [LO + 5, C1], wall: W1 },
];
// Every step above the ground floor stands over the slab's hole: the wall beside it is all that holds it.
const holding = wall => new Set(FLIGHTS.filter(f => f.wall === wall).flatMap(f => f.steps.map(([i]) => i)));

// A door into each room, as near its middle as it can be without taking a stair's wall away.
const doorsIn = wall => ROOMS.map(([i0, i1]) => {
  const mid = Math.floor((i0 + i1) / 2), held = holding(wall);
  for (let d = 0; d <= i1 - i0; d++) for (const i of [mid + d, mid - d]) if (i >= i0 && i <= i1 && !held.has(i)) return i;
  return mid;
});
const SOUTH_DOORS = doorsIn(W0), NORTH_DOORS = doorsIn(W1);

const SLAB = ["wood", "stone", "grass", "dirt", "wood", "stone", "grass", "dirt", "wood", "stone"];
const OUTER = s => (s < Math.ceil(STOREYS / 2) ? "brick" : "gold");
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
const DEPTH = W0 - 1 - LO;                        // rows in a room, back wall to corridor wall
const TABLE_DEEP = Math.min(5, DEPTH - 2);        // the table: past the furniture, a row clear of the door

/** The rooms, as zones: name, columns, rows, and the table in the middle the builder climbs from. */
export const ROOM_ZONES = ROOMS.flatMap(([i0, i1], n) => [
  { name: `room S${n + 1}`, i0, i1, j0: LO + 1, j1: W0 - 1, table: [Math.floor((i0 + i1) / 2), LO + 1 + TABLE_DEEP] },
  { name: `room N${n + 1}`, i0, i1, j0: W1 + 1, j1: HI - 1, table: [Math.floor((i0 + i1) / 2), HI - 1 - TABLE_DEEP] },
]);
export const ZONES = [...ROOM_ZONES.map(z => z.name), "corridor W", "corridor E"];

function roomAt(i, j) { return ROOM_ZONES.find(z => i >= z.i0 && i <= z.i1 && j >= z.j0 && j <= z.j1)?.name ?? null; }
const inCorridor = (i, j) => j >= C0 && j <= C1 && i > LO && i < HI;

/** Whose a column is: a room's inside, the corridor, or a wall, which goes to a room beside it (it is seen from there). */
export function zoneOf(i, j) {
  const own = roomAt(i, j);
  if (own) return own;
  const corridor = i < MID ? "corridor W" : "corridor E";
  // the corridor, and the stretches of its walls the stairs stand against (the rest of its walls go to the rooms)
  if (inCorridor(i, j)) return corridor;
  // where a corridor wall meets the outer wall: seen from the corridor's end, not from the room beside it
  if ((j === W0 || j === W1) && (i === LO || i === HI)) return corridor;
  for (const f of FLIGHTS) if (j === f.wall && i >= Math.min(...f.steps.map(([a]) => a)) - 1 && i <= f.landing[0]) return corridor;
  const four = [[0, -1], [0, 1], [-1, 0], [1, 0]].map(([di, dj]) => roomAt(i + di, j + dj)).filter(Boolean);
  if (four.length) return four[0];
  if ([[0, -1], [0, 1], [-1, 0], [1, 0]].some(([di, dj]) => inCorridor(i + di, j + dj))) return corridor;
  const eight = [[-1, -1], [1, -1], [-1, 1], [1, 1]].map(([di, dj]) => roomAt(i + di, j + dj)).filter(Boolean);
  return eight[0] ?? corridor;
}

/**
 * Every block of the house in a region whose south-west corner is (rx, ry), as
 * { x, y, z, kind, storey, phase, role, zone }. The ground of the whole region is part of the plan too (storey -1).
 */
export function housePlan(rx, ry) {
  const blocks = new Map();
  const at = (i, j, z, kind, storey, phase, role) => blocks.set(`${rx + i}:${ry + j}:${z}`, { x: rx + i, y: ry + j, z, kind, storey, phase, role, zone: zoneOf(i, j) });
  const off = (i, j, z) => blocks.delete(`${rx + i}:${ry + j}:${z}`);
  const edge = (i, j) => i === LO || i === HI || j === LO || j === HI;
  const corner = (i, j) => (i === LO || i === HI) && (j === LO || j === HI);
  const ring = function* () { for (let t = 0; t < SIZE; t++) for (const [i, j] of [[LO + t, LO], [LO + t, HI], [LO, LO + t], [HI, LO + t]]) yield [t, i, j]; };

  for (let i = 0; i < 24; i++) for (let j = 0; j < 24; j++)
    for (const z of [-3, -2, -1]) at(i, j, z, z === -1 ? "grass" : "dirt", -1, 0, "ground");

  for (let s = 0; s < STOREYS; s++) {
    const base = s * H;
    for (let dz = 0; dz < 3; dz++) {
      const z = base + dz;
      for (const [t, i, j] of ring()) {
        if (corner(i, j)) at(i, j, z, OUTER(s), s, 0, "corner");
        else at(i, j, z, dz === 1 && t % 4 === 2 ? "glass" : OUTER(s), s, 1, "facade");
      }
      for (let i = LO + 1; i < HI; i++) { at(i, W0, z, INNER(s), s, 1, "inner"); at(i, W1, z, INNER(s), s, 1, "inner"); }
      for (const i of PARTITIONS) for (let j = LO + 1; j < HI; j++) if (j < W0 || j > W1) at(i, j, z, INNER(s), s, 1, "inner");
    }
    for (const dz of [0, 1]) { for (const i of SOUTH_DOORS) off(i, W0, base + dz); for (const i of NORTH_DOORS) off(i, W1, base + dz); }
    // Where two walls meet, a junction: it goes in first.
    for (let dz = 0; dz < 3; dz++) {
      const z = base + dz, wall = (i, j) => { const b = blocks.get(`${rx + i}:${ry + j}:${z}`); return b && (b.role === "facade" || b.role === "inner" || b.role === "corner"); };
      for (let i = LO; i <= HI; i++) for (let j = LO; j <= HI; j++) {
        const b = blocks.get(`${rx + i}:${ry + j}:${z}`);
        if (!b || (b.role !== "facade" && b.role !== "inner")) continue;
        if ((wall(i - 1, j) || wall(i + 1, j)) && (wall(i, j - 1) || wall(i, j + 1))) Object.assign(b, { phase: 0, role: "junction" });
      }
    }
    // A junction's column goes in whole, from the floor: its upper block cannot go in before the ones it stands on
    // (a door beside the lower two makes only the top one a junction).
    for (let i = LO; i <= HI; i++) for (let j = LO; j <= HI; j++) {
      const col = [0, 1, 2].map(dz => blocks.get(`${rx + i}:${ry + j}:${base + dz}`));
      if (col.some(b => b?.role === "junction")) for (const b of col) if (b && (b.role === "facade" || b.role === "inner")) Object.assign(b, { phase: 0, role: "junction" });
    }
    // Furniture along each room's back wall, and the table in the middle. In a corner room nothing stands in the cell
    // beside the house's corner: the corner is seen from there.
    ROOMS.forEach(([i0, i1], n) => {
      for (const side of ["south", "north"]) {
        const kind = ROOM_KINDS[(s * 3 + n * 2 + (side === "north" ? 1 : 0)) % ROOM_KINDS.length];
        const cornerRoom = n === 0 || n === ROOMS.length - 1;
        for (const [a, d, u, k] of FURNITURE[kind]) {
          if (a > i1 - i0 || d > TABLE_DEEP - 1) continue;
          const i = side === "south" ? i0 + a : i1 - a, j = side === "south" ? LO + 1 + d : HI - 1 - d;
          if (cornerRoom && d === 0 && (i === LO + 1 || i === HI - 1)) continue;
          at(i, j, base + u, k, s, 2, "furniture");
        }
      }
    });
    for (const z of ROOM_ZONES) at(z.table[0], z.table[1], base, "wood", s, 2, "table");
    // The slab, open over both flights; the hatch over every table; the steps, each against its corridor wall.
    const slabKind = s === STOREYS - 1 ? "stone" : SLAB[s % SLAB.length];
    // Over an inner wall the slab goes in with the rest, but before both its neighbours (crew.mjs): once they are in,
    // it is seen from above only, and the wall of the storey above covers it.
    const overWall = (i, j) => ["inner", "junction"].includes(blocks.get(`${rx + i}:${ry + j}:${base + 2}`)?.role);
    for (let i = LO; i <= HI; i++) for (let j = LO; j <= HI; j++)
      at(i, j, base + 3, slabKind, s, 3, corner(i, j) ? "slab corner" : edge(i, j) ? "slab edge" : overWall(i, j) ? "slab over wall" : "slab");
    for (const z of ROOM_ZONES) at(z.table[0], z.table[1], base + 3, slabKind, s, 4, "hatch");
    for (const f of FLIGHTS) f.steps.forEach(([i, j], n) => { at(i, j, base + n, "wood", s, 1, "stairs"); off(i, j, base + 3); });
  }
  // The ground floor's front door, west; a railing round the roof.
  for (const j of [C0, C1]) for (const z of [0, 1]) off(LO, j, z);
  for (const [, i, j] of ring()) at(i, j, ROOF + 1, "glass", STOREYS - 1, 5, "railing");
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
