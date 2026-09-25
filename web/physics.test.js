// node --test web/
import { test } from "node:test";
import assert from "node:assert/strict";
import { createBody, tick, knockback } from "./physics.js";
import * as S from "./spec.js";

const flat = (x, y, z) => y < 0;
const idle = { forward: 0, strafe: 0, jump: false, sneak: false, sprint: false, yaw: 0 };
const run = (body, input, world, ticks) => { for (let i = 0; i < ticks; i++) tick(body, input, world); return body; };
const settle = body => run(body, idle, flat, 5);

test("a jump rises 1.2522 blocks", () => {
  const body = settle(createBody(5, 0, 5));
  let peak = 0;
  for (let i = 0; i < 30; i++) { tick(body, { ...idle, jump: i === 0 }, flat); peak = Math.max(peak, body.y); }
  assert.ok(Math.abs(peak - 1.2522) < 0.001, `peak ${peak}`);
  assert.equal(body.onGround, true);
  assert.equal(body.y, 0);
});

test("holding jump waits ten ticks between jumps", () => {
  const body = settle(createBody(5, 0, 5));
  const jumps = [];
  for (let i = 0; i < 60; i++) { const before = body.onGround; tick(body, { ...idle, jump: true }, flat); if (before && body.vy > 0.3) jumps.push(i); }
  assert.ok(jumps.length >= 2);
  assert.ok(jumps[1] - jumps[0] >= S.JUMP_DELAY_TICKS, `jumps at ${jumps}`);
});

test("walking settles at 4.317 m/s, sprinting at 5.612, sneaking at 1.295", () => {
  const speed = input => {
    const body = run(settle(createBody(5, 0, 5)), input, flat, 200);
    tick(body, input, flat);
    return Math.hypot(body.x - body.px, body.z - body.pz) * S.TPS;
  };
  assert.ok(Math.abs(speed({ ...idle, forward: 1 }) - 4.317) < 0.002);
  assert.ok(Math.abs(speed({ ...idle, forward: 1, sprint: true }) - 5.612) < 0.002);
  assert.ok(Math.abs(speed({ ...idle, forward: 1, sneak: true }) - 1.295) < 0.002);
  // A diagonal input is normalised, so it is not faster than the two keys' sum would make it (it loses the 0.98 keyboard scale).
  const diagonal = speed({ ...idle, forward: 1, strafe: 1 });
  assert.ok(diagonal > 4.3 && diagonal < 4.41, `diagonal ${diagonal}`);
});

test("forward follows the yaw: yaw 0 walks toward +z", () => {
  const body = run(settle(createBody(5, 0, 5)), { ...idle, forward: 1 }, flat, 20);
  assert.ok(body.z > 5.5 && Math.abs(body.x - 5) < 1e-9);
  const east = run(settle(createBody(5, 0, 5)), { ...idle, forward: 1, yaw: -Math.PI / 2 }, flat, 20);
  assert.ok(east.x > 5.5 && Math.abs(east.z - 5) < 1e-9);
});

test("falling reaches the terminal velocity of 3.92 blocks a tick", () => {
  const body = run(createBody(5, 500, 5), idle, () => false, 400);
  assert.ok(Math.abs(body.vy + 3.92) < 0.01, `vy ${body.vy}`);
});

test("a wall stops the player and zeroes that velocity", () => {
  const wall = (x, y, z) => y < 0 || (z === 7 && y < 3);
  const body = run(settle(createBody(5, 0, 5)), { ...idle, forward: 1, sprint: true }, wall, 60);
  assert.ok(body.z <= 7 - S.WIDTH / 2 + 1e-9 && body.z > 6.6, `z ${body.z}`);
  assert.equal(body.vz, 0);
  assert.equal(body.horizontalCollision, true);
});

test("a full block is not stepped over, it must be jumped", () => {
  const step = (x, y, z) => y < 0 || (z >= 7 && y === 0);
  const walked = run(settle(createBody(5, 0, 5)), { ...idle, forward: 1 }, step, 60);
  assert.ok(walked.z < 7, `walked ${walked.z}`);
  const jumped = settle(createBody(5, 0, 6.5));
  tick(jumped, { ...idle, forward: 1, jump: true }, step);
  run(jumped, { ...idle, forward: 1 }, step, 20);
  assert.ok(jumped.onGround && jumped.y === 1 && jumped.z > 6.7, `jumped to ${jumped.z}, ${jumped.y}`);
});

test("sneaking at an edge does not fall off", () => {
  const pillar = (x, y, z) => y < 0 && x >= 4 && x < 6 && z >= 4 && z < 6;
  const body = run(settle(createBody(5, 0, 5)), { ...idle, forward: 1, sneak: true }, pillar, 100);
  assert.equal(body.y, 0);
  // The hitbox may hang over the edge as long as part of it is still above the pillar.
  assert.ok(body.z < 6 + S.WIDTH / 2 && body.z > 5.5, `z ${body.z}`);
  const walker = run(settle(createBody(5, 0, 5)), { ...idle, forward: 1 }, pillar, 100);
  assert.ok(walker.y < -1, "without sneaking the player falls off");
});

test("sneaking lowers the hitbox, sprinting needs forward input and no sneaking", () => {
  const body = settle(createBody(5, 0, 5));
  tick(body, { ...idle, sneak: true, sprint: true, forward: 1 }, flat);
  assert.equal(body.sneaking, true);
  assert.equal(body.sprinting, false);
  tick(body, { ...idle, sprint: true }, flat);
  assert.equal(body.sprinting, false);
  tick(body, { ...idle, sprint: true, forward: 1 }, flat);
  assert.equal(body.sprinting, true);
});

test("knockback halves the motion, pushes and lifts a grounded victim", () => {
  const body = settle(createBody(5, 0, 5));
  body.vx = 0.2;
  knockback(body, 0.4, 0, 0.4);
  assert.ok(Math.abs(body.vx - 0.5) < 1e-9);
  // A standing player already carries one tick of gravity (-0.0784), halved before the lift is added.
  assert.ok(body.vy > 0.35 && body.vy <= S.KNOCKBACK_LIFT, `vy ${body.vy}`);
});
