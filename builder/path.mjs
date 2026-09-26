// Where a builder can walk: a breadth-first search over the cells a player can stand in. A cell is a place to
// stand when the feet and the head are free and the block under the feet is solid. From one, a player walks to
// the next cell along x or y on the same level, jumps one block up (the jump needs two free blocks over the head
// where it takes off and lands, or the client's physics stops it short), or walks off and lands up to MAX_DROP
// below (a player takes a heart a block past the third, and heals).

import { key } from "./world.mjs";

export const MAX_DROP = 6;
const STEPS = [[1, 0], [-1, 0], [0, 1], [0, -1]];

export function standable(world, inside, x, y, z) {
  return inside(x, y) && world.solid(x, y, z - 1) && !world.solid(x, y, z) && !world.solid(x, y, z + 1);
}

export function moves(world, inside, c) {
  const out = [];
  for (const [dx, dy] of STEPS) {
    const x = c.x + dx, y = c.y + dy;
    if (!inside(x, y)) continue;
    if (standable(world, inside, x, y, c.z)) { out.push({ x, y, z: c.z }); continue; }
    if (standable(world, inside, x, y, c.z + 1) && !world.solid(c.x, c.y, c.z + 2) && !world.solid(x, y, c.z + 2)) { out.push({ x, y, z: c.z + 1 }); continue; }
    if (world.solid(x, y, c.z) || world.solid(x, y, c.z + 1)) continue;
    let z = c.z;
    while (z > c.z - MAX_DROP - 1 && !world.solid(x, y, z - 1)) z--;
    if (z >= c.z - MAX_DROP && world.solid(x, y, z - 1)) out.push({ x, y, z });
  }
  return out;
}

/** Every cell reachable from a cell, with its distance and the way back. */
export function reach(world, inside, from) {
  const dist = new Map([[key(from.x, from.y, from.z), 0]]), prev = new Map(), cells = [from];
  for (let i = 0; i < cells.length; i++) {
    const c = cells[i], d = dist.get(key(c.x, c.y, c.z));
    for (const n of moves(world, inside, c)) {
      const k = key(n.x, n.y, n.z);
      if (dist.has(k)) continue;
      dist.set(k, d + 1); prev.set(k, c); cells.push(n);
    }
  }
  return { dist, prev, cells };
}

export function pathTo(r, to) {
  const path = [];
  for (let c = to; c; c = r.prev.get(key(c.x, c.y, c.z))) path.unshift(c);
  return path.slice(1);
}
