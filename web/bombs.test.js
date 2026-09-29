// node --test web/
import { test } from "node:test";
import assert from "node:assert/strict";
import { descend, fly, inPickupReach, explode, blastDamage } from "./bombs.js";
import * as S from "./spec.js";

const flat = (x, y, z) => y < 0;
const bounds = { width: 72, depth: 24, minY: -4 };

test("a parachute comes down two blocks a second and rests on the ground", () => {
  let y = S.DROP_HEIGHT;
  for (let t = 0; t < S.TPS; t++) y = descend(flat, 30.5, y, 12.5);
  assert.ok(Math.abs(y - (S.DROP_HEIGHT - 2)) < 1e-6, `y ${y}`);
  for (let t = 0; t < 1000; t++) y = descend(flat, 30.5, y, 12.5);
  assert.equal(y, 0);
});

test("a thrown bomb lands 12 to 22 blocks away, as the server flies it", () => {
  const p = { x: 10, y: 1.62, z: 12 }, v = { x: Math.cos(0.5), y: Math.sin(0.5), z: 0 };
  let result = "flying";
  for (let age = 0; result === "flying" && age < 200; age++) result = fly(flat, bounds, p, v, [], "me", age);
  assert.equal(result, "exploded");
  assert.ok(p.x > 22 && p.x < 32, `x ${p.x}`);
});

test("a bomb off the edge of the world is gone", () => {
  assert.equal(fly(flat, bounds, { x: 0.5, y: 1.5, z: 12 }, { x: -1, y: 0, z: 0 }), "gone");
});

test("a bomb is picked up within a block of the hitbox", () => {
  const body = { x: 30, y: 0, z: 12 };
  assert.ok(inPickupReach(body, S.HEIGHT, 31.2, 0, 12));
  assert.ok(!inPickupReach(body, S.HEIGHT, 31.5, 0, 12));
});

test("an explosion breaks dirt but not bedrock", () => {
  const block = (x, y, z) => y === -4 ? { resistance: 3600000, breakable: false } : y < 0 ? { resistance: 0.5, breakable: true } : null;
  const destroyed = explode(block, 30.5, 0, 12.5, S.BOMB_POWER, () => 0.5);
  assert.ok(destroyed.length > 20);
  assert.ok(destroyed.every(([, y]) => y > -4));
});

test("a blast hurts least at its reach and not beyond", () => {
  assert.equal(blastDamage(0, S.BOMB_POWER).damage, 43);
  assert.equal(blastDamage(S.BLAST_REACH, S.BOMB_POWER).damage, 1);
  assert.equal(blastDamage(S.BLAST_REACH + 0.1, S.BOMB_POWER), null);
});
