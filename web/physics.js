// Player movement, one 50 ms tick at a time, as Minecraft's LivingEntity does it:
// accelerate from the input, move against the blocks, then apply friction and gravity.
// Coordinates here are the client's: x and z on the ground, y up; the body position is the feet.

import * as S from "./spec.js";

const EPS = 1e-7;

export function createBody(x, y, z) {
  return { x, y, z, px: x, py: y, pz: z, vx: 0, vy: 0, vz: 0,
    onGround: false, sneaking: false, sprinting: false, jumpDelay: 0, horizontalCollision: false };
}

export function bodyHeight(body) {
  return body.sneaking ? S.SNEAK_HEIGHT : S.HEIGHT;
}

export function eyeHeight(body) {
  return body.sneaking ? S.SNEAK_EYE_HEIGHT : S.EYE_HEIGHT;
}

// input: { forward: -1..1, strafe: -1..1, jump, sneak, sprint, yaw } — yaw is Minecraft's: 0 faces +z,
// forward is (-sin yaw, cos yaw). isSolid(x, y, z) answers for integer block coordinates.
export function tick(body, input, isSolid) {
  body.px = body.x; body.py = body.y; body.pz = body.z;
  body.sneaking = !!input.sneak;
  body.sprinting = !!input.sprint && input.forward > 0 && !body.sneaking;

  // Jump: 0.42 up, and 0.2 forward when sprinting.
  if (input.jump) {
    if (body.onGround && body.jumpDelay === 0) {
      body.vy = S.JUMP_VELOCITY;
      if (body.sprinting) {
        body.vx += -Math.sin(input.yaw) * S.SPRINT_JUMP_BOOST;
        body.vz += Math.cos(input.yaw) * S.SPRINT_JUMP_BOOST;
      }
      body.jumpDelay = S.JUMP_DELAY_TICKS;
    }
  } else body.jumpDelay = 0;
  if (body.jumpDelay > 0) body.jumpDelay--;

  // Acceleration from the keys.
  let strafe = input.strafe * S.INPUT_SCALE, forward = input.forward * S.INPUT_SCALE;
  if (body.sneaking) { strafe *= S.SNEAK_MULTIPLIER; forward *= S.SNEAK_MULTIPLIER; }
  const lengthSq = strafe * strafe + forward * forward;
  if (lengthSq >= 1e-7) {
    let speed = body.onGround ? S.WALK_ACCELERATION : S.AIR_ACCELERATION;
    if (body.sprinting) speed *= S.SPRINT_MULTIPLIER;
    const scale = (lengthSq > 1 ? 1 / Math.sqrt(lengthSq) : 1) * speed;
    const sx = strafe * scale, sz = forward * scale;
    const sin = Math.sin(input.yaw), cos = Math.cos(input.yaw);
    body.vx += sx * cos - sz * sin;
    body.vz += sz * cos + sx * sin;
  }

  // Sneaking on the ground keeps the player from walking off an edge.
  let dx = body.vx, dy = body.vy, dz = body.vz;
  if (body.sneaking && body.onGround) [dx, dz] = backOffFromEdge(body, dx, dz, isSolid);

  // Move: y first, then the larger horizontal axis.
  const wasGoingDown = dy < 0;
  const moved = collide(body, dx, dy, dz, isSolid);
  body.x += moved.x; body.y += moved.y; body.z += moved.z;
  const collidedY = Math.abs(moved.y - dy) > EPS;
  body.onGround = collidedY && wasGoingDown;
  body.horizontalCollision = Math.abs(moved.x - dx) > EPS || Math.abs(moved.z - dz) > EPS;
  if (Math.abs(moved.x - dx) > EPS) body.vx = 0;
  if (Math.abs(moved.z - dz) > EPS) body.vz = 0;
  if (collidedY) body.vy = 0;

  // Friction and gravity, after the move.
  const friction = body.onGround ? S.GROUND_FRICTION : S.AIR_FRICTION;
  body.vy = (body.vy - S.GRAVITY) * S.VERTICAL_DRAG;
  body.vx *= friction;
  body.vz *= friction;
  if (Math.abs(body.vx) < S.MIN_VELOCITY) body.vx = 0;
  if (Math.abs(body.vy) < S.MIN_VELOCITY) body.vy = 0;
  if (Math.abs(body.vz) < S.MIN_VELOCITY) body.vz = 0;
  return body;
}

// Knockback from a hit: halve the current motion, push away, lift a grounded victim.
export function knockback(body, kx, kz, strength) {
  body.vx = body.vx / 2 + kx;
  body.vz = body.vz / 2 + kz;
  if (body.onGround) body.vy = Math.min(S.KNOCKBACK_LIFT, body.vy / 2 + strength);
}

// Entities push each other apart when their hitboxes overlap.
export function pushAway(body, others) {
  for (const o of others) {
    const h = bodyHeight(body);
    if (o.y >= body.y + h || o.y + (o.height ?? S.HEIGHT) <= body.y) continue;
    let dx = body.x - o.x, dz = body.z - o.z;
    const d = Math.max(Math.abs(dx), Math.abs(dz));
    if (d >= S.WIDTH || d < 0.01) continue;
    dx /= d; dz /= d;
    const f = Math.min(1, 1 / d);
    body.vx += dx * f * S.PUSH;
    body.vz += dz * f * S.PUSH;
  }
}

function box(body) {
  const half = S.WIDTH / 2;
  return { min: [body.x - half, body.y, body.z - half], max: [body.x + half, body.y + bodyHeight(body), body.z + half] };
}

function collide(body, dx, dy, dz, isSolid) {
  const b = box(body);
  const out = { x: 0, y: 0, z: 0 };
  out.y = sweep(b, 1, dy, isSolid);
  shift(b, 1, out.y);
  const zFirst = Math.abs(dz) > Math.abs(dx);
  if (zFirst) { out.z = sweep(b, 2, dz, isSolid); shift(b, 2, out.z); }
  out.x = sweep(b, 0, dx, isSolid);
  shift(b, 0, out.x);
  if (!zFirst) { out.z = sweep(b, 2, dz, isSolid); shift(b, 2, out.z); }
  return out;
}

function shift(b, axis, d) {
  b.min[axis] += d;
  b.max[axis] += d;
}

// How far the box can move along an axis before a solid block stops it.
function sweep(b, axis, d, isSolid) {
  if (d === 0) return 0;
  const lo = [0, 1, 2].map(a => Math.floor(b.min[a] + EPS));
  const hi = [0, 1, 2].map(a => Math.floor(b.max[a] - EPS));
  const edge = d > 0 ? b.max[axis] : b.min[axis];
  const from = Math.floor(Math.min(edge, edge + d) + EPS), to = Math.floor(Math.max(edge, edge + d) - EPS);
  const other = [0, 1, 2].filter(a => a !== axis);
  const positive = d > 0;
  for (let i = from; i <= to; i++)
    for (let j = lo[other[0]]; j <= hi[other[0]]; j++)
      for (let k = lo[other[1]]; k <= hi[other[1]]; k++) {
        const p = [0, 0, 0];
        p[axis] = i; p[other[0]] = j; p[other[1]] = k;
        if (!isSolid(p[0], p[1], p[2])) continue;
        d = positive ? Math.min(d, i - b.max[axis]) : Math.max(d, i + 1 - b.min[axis]);
      }
  return Math.abs(d) < EPS ? 0 : d;
}

function backOffFromEdge(body, dx, dz, isSolid) {
  const b = box(body);
  // "Free" means the box, moved and lowered by the step height, touches nothing: there is no ground there.
  const free = (ox, oz) => !overlaps({
    min: [b.min[0] + ox, b.min[1] - S.STEP_HEIGHT, b.min[2] + oz],
    max: [b.max[0] + ox, b.max[1] - S.STEP_HEIGHT, b.max[2] + oz],
  }, isSolid);
  const shrink = v => Math.abs(v) < 0.05 ? 0 : v - Math.sign(v) * 0.05;
  while (dx !== 0 && free(dx, 0)) dx = shrink(dx);
  while (dz !== 0 && free(0, dz)) dz = shrink(dz);
  while (dx !== 0 && dz !== 0 && free(dx, dz)) { dx = shrink(dx); dz = shrink(dz); }
  return [dx, dz];
}

function overlaps(b, isSolid) {
  for (let x = Math.floor(b.min[0] + EPS); x <= Math.floor(b.max[0] - EPS); x++)
    for (let y = Math.floor(b.min[1] + EPS); y <= Math.floor(b.max[1] - EPS); y++)
      for (let z = Math.floor(b.min[2] + EPS); z <= Math.floor(b.max[2] - EPS); z++)
        if (isSolid(x, y, z)) return true;
  return false;
}
