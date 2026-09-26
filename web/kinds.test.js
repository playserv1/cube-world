// node --test web/
import { test } from "node:test";
import assert from "node:assert/strict";
import { kindOf, inventoryOf, withKinds } from "./kinds.js";

test("a kind from the wire is taken without regard to case", () => {
  assert.equal(kindOf("Stone"), "stone");
  assert.equal(kindOf("stone"), "stone");
  assert.equal(kindOf(null), null);
});

test("one kind under two spellings adds up, within a stack", () => {
  assert.deepEqual(inventoryOf({ grass: 64, Stone: 30, stone: 3 }), { grass: 64, stone: 33 });
  assert.deepEqual(inventoryOf({ Stone: 64, stone: 1 }), { stone: 64 });
  assert.deepEqual(inventoryOf(undefined), {});
});

test("every block kind in a frame comes in this client's spelling", () => {
  const welcome = withKinds({
    type: "welcome", hotbar: ["grass", "Stone"], inventory: { Stone: 64 },
    world: [{ x: 1, y: 2, z: 3, kind: "Stone" }], blocks: [{ kind: "Stone", Drop: null }, { kind: "grass", Drop: "Dirt" }],
    layers: [{ z: -1, kind: "grass" }],
  });
  assert.deepEqual(welcome.hotbar, ["grass", "stone"]);
  assert.deepEqual(welcome.inventory, { stone: 64 });
  assert.equal(welcome.world[0].kind, "stone");
  assert.equal(welcome.blocks[0].kind, "stone");
  assert.equal(welcome.blocks[0].Drop, null);
  assert.equal(welcome.blocks[1].Drop, "dirt");

  const cubes = withKinds({ type: "cubes", changes: [{ op: "upsert", cube: { kind: "Stone" } }, { op: "delete", cube: { x: 0 } }], falls: [{ kind: "Stone" }] });
  assert.equal(cubes.changes[0].cube.kind, "stone");
  assert.equal(cubes.changes[1].cube.kind, undefined);
  assert.equal(cubes.falls[0].kind, "stone");
  assert.equal(withKinds({ type: "cube", op: "upsert", cube: { kind: "Stone" } }).cube.kind, "stone");
  assert.equal(withKinds({ type: "fall", kind: "Stone" }).kind, "stone");
  assert.deepEqual(withKinds({ type: "players", players: [] }), { type: "players", players: [] });
});
