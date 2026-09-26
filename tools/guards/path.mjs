// Where a guard can walk, and the shortest way there: a breadth-first search over the cells a player can stand
// in. A cell is a place to stand when the feet and the head are free and the block under the feet is solid. From
// one, a player walks to the next cell along x or y on the same level, jumps one block up, or drops up to three.

const STEPS = [[1, 0], [-1, 0], [0, 1], [0, -1]];
const MAX_NODES = 20000;

export function standable(solid, x, y, z) {
  return !solid(x, y, z) && !solid(x, y, z + 1) && solid(x, y, z - 1);
}

/** The cell the feet are in, or the nearest one below to stand in (a player in the air lands there). */
export function standingCell(solid, p) {
  const x = Math.floor(p.x), y = Math.floor(p.y);
  let z = Math.floor(p.z + 1e-3);
  for (let i = 0; i < 6 && !standable(solid, x, y, z); i++) z--;
  return standable(solid, x, y, z) ? { x, y, z } : { x, y, z: Math.floor(p.z + 1e-3) };
}

function* neighbours(solid, { x, y, z }) {
  for (const [dx, dy] of STEPS) {
    const nx = x + dx, ny = y + dy;
    if (standable(solid, nx, ny, z)) { yield { x: nx, y: ny, z }; continue; }
    if (standable(solid, nx, ny, z + 1) && !solid(x, y, z + 2)) { yield { x: nx, y: ny, z: z + 1 }; continue; }
    // Walk off the edge: the body passes through the next column at its own height, then falls.
    if (solid(nx, ny, z) || solid(nx, ny, z + 1)) continue;
    for (let dz = 1; dz <= 3; dz++) {
      if (solid(nx, ny, z - dz + 1) && dz > 1) break;
      if (standable(solid, nx, ny, z - dz)) { yield { x: nx, y: ny, z: z - dz }; break; }
    }
  }
}

/**
 * The cells from `from` to the nearest cell `goal` accepts, both ends included; null when none is reachable.
 * `inside(cell)` keeps the search within an area.
 */
export function findPath(solid, from, goal, inside = () => true) {
  const k = c => `${c.x}:${c.y}:${c.z}`;
  const came = new Map([[k(from), null]]);
  const queue = [from];
  for (let i = 0; i < queue.length && i < MAX_NODES; i++) {
    const cell = queue[i];
    if (goal(cell)) {
      const path = [];
      for (let c = cell; c; c = came.get(k(c))) path.push(c);
      return path.reverse();
    }
    for (const next of neighbours(solid, cell)) {
      if (came.has(k(next)) || !inside(next)) continue;
      came.set(k(next), cell);
      queue.push(next);
    }
  }
  return null;
}
