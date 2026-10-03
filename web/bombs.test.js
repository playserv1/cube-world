// node --test web/
import { test } from "node:test";
import assert from "node:assert/strict";
import { descend, fly, inPickupReach, explode, blastDamage, isStale, goneAtWelcome } from "./bombs.js";
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

test("an explosion breaks the block under it and one around, no deeper", () => {
  const block = (x, y, z) => y === -4 ? { resistance: 3600000, breakable: false } : y < 0 ? { resistance: 0.5, breakable: true } : null;
  const destroyed = explode(block, 30.5, 0.3, 12.5, S.CRATER_POWER, () => 0.5);
  assert.equal(destroyed.length, 9);
  assert.ok(destroyed.every(([x, y, z]) => y === -1 && Math.abs(x - 30) <= 1 && Math.abs(z - 12) <= 1));
});

test("a record behind what the client shows is stale; one level with it or ahead is not", () => {
  assert.ok(isStale("flying", "held"));
  assert.ok(isStale("flying", "free"));
  assert.ok(isStale("held", "free"));
  assert.ok(!isStale("flying", "flying"));
  assert.ok(!isStale("held", "flying"));
  assert.ok(!isStale("free", "held"));
  assert.ok(!isStale(undefined, "free"), "a bomb the client does not show takes any record");
});

// What the welcome case of app.js's onFrame does with the bombs the client shows (id → state).
function welcome(shown, listed) {
  for (const id of goneAtWelcome(shown.keys(), listed)) shown.delete(id);
  for (const { bomb } of listed) if (!isStale(shown.get(bomb.bomb_id), bomb.state)) shown.set(bomb.bomb_id, bomb.state);
  return Object.fromEntries(shown);
}

test("a bomb thrown just before a crossing flies on when the next server still lists it in the hand (PSV-3033)", () => {
  const shown = new Map([["thrown", "flying"], ["picked", "held"], ["lying", "free"]]);
  const listed = [
    { bomb: { bomb_id: "thrown", state: "held" }, age: 900 },   // the Unreal server's flying row has not reached this server yet
    { bomb: { bomb_id: "picked", state: "flying" }, age: 40 },
    { bomb: { bomb_id: "new", state: "free" }, age: 3000 },
  ];
  assert.deepEqual(welcome(shown, listed), { thrown: "flying", picked: "flying", new: "free" });
});

test("a welcome takes away the bombs it does not list: they went off or fizzled while the client was elsewhere", () => {
  assert.deepEqual(goneAtWelcome(["a", "b", "c"], [{ bomb: { bomb_id: "b", state: "free" } }]), ["a", "c"]);
  assert.deepEqual(goneAtWelcome(new Map([["a", 1]]).keys(), []), ["a"]);
  assert.deepEqual(welcome(new Map([["a", "flying"]]), []), {});
});

test("a blast hurts least at its reach and not beyond", () => {
  assert.equal(blastDamage(0, S.BOMB_POWER).damage, 43);
  assert.equal(blastDamage(S.BLAST_REACH, S.BOMB_POWER).damage, 1);
  assert.equal(blastDamage(S.BLAST_REACH + 0.1, S.BOMB_POWER), null);
});
