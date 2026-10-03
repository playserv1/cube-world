// How a bomb moves, the same rules as CubeWorld.Server/Bomb.cs and World.Explode. The client keeps y up,
// so a bomb's height is y here. The server decides; the client follows the same path to draw it.

import * as S from "./spec.js";

// One tick under the parachute: down 0.1 until the bomb rests on a block; out on top of a block put on it.
export function descend(isSolid, x, y, z) {
  const bx = Math.floor(x), bz = Math.floor(z);
  if (isSolid(bx, Math.floor(y), bz)) return Math.floor(y) + 1;
  const next = y - S.PARACHUTE_SPEED;
  const below = Math.floor(next);
  return isSolid(bx, below, bz) ? below + 1 : next;
}

// One tick of a thrown bomb: "flying", "exploded" (at the last free point, p) or "gone" off the world.
export function fly(isSolid, bounds, p, v, players = [], owner = null, age = 0) {
  const length = Math.hypot(v.x, v.y, v.z);
  const steps = Math.max(1, Math.ceil(length / 0.1));
  for (let i = 1; i <= steps; i++) {
    const x = p.x + v.x / steps, y = p.y + v.y / steps, z = p.z + v.z / steps;
    if (x < 0 || x >= bounds.width || z < 0 || z >= bounds.depth || y < bounds.minY) return "gone";
    if (isSolid(Math.floor(x), Math.floor(y), Math.floor(z))) return "exploded";
    p.x = x; p.y = y; p.z = z;
    for (const o of players)
      if ((o.id !== owner || age >= S.OWNER_IMMUNITY_TICKS) && Math.abs(x - o.x) <= S.WIDTH / 2 && Math.abs(z - o.z) <= S.WIDTH / 2
          && y >= o.y && y <= o.y + (o.height ?? S.HEIGHT)) return "exploded";
  }
  v.x *= S.PROJECTILE_DRAG; v.y *= S.PROJECTILE_DRAG; v.z *= S.PROJECTILE_DRAG;
  v.y -= S.PROJECTILE_GRAVITY;
  return "flying";
}

export function inPickupReach(body, height, x, y, z) {
  const reach = S.WIDTH / 2 + S.PICKUP_REACH;
  return Math.abs(x - body.x) <= reach && Math.abs(z - body.z) <= reach && y >= body.y - S.PICKUP_REACH_UP && y <= body.y + height + S.PICKUP_REACH_UP;
}

// A bomb only moves forward (free, held, flying), then it is over. A record behind what the client shows is stale and
// changes nothing: it never takes a bomb out of the hand, nor a thrown one back into it.
const BOMB_RANK = { free: 0, held: 1, flying: 2 };

export function isStale(shown, state) {
  return shown !== undefined && BOMB_RANK[state] < BOMB_RANK[shown];
}

// The bombs a new server's welcome takes away: those it does not list went off or fizzled while the client was
// elsewhere. The others stay, and the list's record of each goes through the rule above like any other frame. A bomb
// thrown on an Unreal server a moment before a crossing is often still "held" in the next server's list, since the
// Unreal server's row reaches it 0.2-0.4 s later; it flies on rather than coming back into the thrower's hand until then
// (PSV-3033). The Unreal client keeps the same rules (ACubePlayerPawn::HandleBombList and HandleBomb).
export function goneAtWelcome(shown, listed) {
  const known = new Set(listed.map(f => f.bomb?.bomb_id));
  return [...shown].filter(id => !known.has(id));
}

// Minecraft's explosion rays (World.Explode): the blocks a blast destroys. resistance(x, y, z) is the block's
// blast resistance, or null for air or a block that cannot break (whose resistance still stops the ray).
export function explode(block, cx, cy, cz, power, random = Math.random) {
  const destroyed = new Map();
  for (let i = 0; i < 16; i++) for (let j = 0; j < 16; j++) for (let k = 0; k < 16; k++) {
    if (i !== 0 && i !== 15 && j !== 0 && j !== 15 && k !== 0 && k !== 15) continue;
    let dx = i / 15 * 2 - 1, dy = j / 15 * 2 - 1, dz = k / 15 * 2 - 1;
    const length = Math.hypot(dx, dy, dz);
    dx /= length; dy /= length; dz /= length;
    let x = cx, y = cy, z = cz;
    for (let intensity = power * (0.7 + random() * 0.6); intensity > 0; intensity -= 0.22500001) {
      const bx = Math.floor(x), by = Math.floor(y), bz = Math.floor(z);
      const b = block(bx, by, bz);
      if (b) {
        intensity -= (b.resistance + 0.3) * 0.3;
        if (intensity > 0 && b.breakable) destroyed.set(`${bx},${by},${bz}`, [bx, by, bz]);
      }
      x += dx * 0.3; y += dy * 0.3; z += dz * 0.3;
    }
  }
  return [...destroyed.values()];
}

// Damage without cover, for the offline world: within BLAST_REACH, ⌊(impact² + impact) / 2 × 7 × 2 × power + 1⌋.
export function blastDamage(distance, power) {
  const d = distance / S.BLAST_REACH;
  if (d > 1) return null;
  const impact = 1 - d;
  return { damage: Math.floor((impact * impact + impact) / 2 * 7 * 2 * power + 1), impact };
}
