// node --test web/
import { test } from "node:test";
import assert from "node:assert/strict";
import { refusal } from "./rooms.js";

test("a room the operator closed is tried again in half a minute, when it has opened fresh", () => {
  for (const why of [{ reason: "room_closed_by_operator" }, { code: "room_closed" }]) {
    const r = refusal(why);
    assert.equal(r.waitMs, 30_000);
    assert.match(r.message, /closed by an operator/);
  }
});

test("a player the operator removed stops knocking on that room and only checks back once a minute", () => {
  for (const why of [{ reason: "removed_by_operator" }, { code: "removed_from_room" }]) {
    const r = refusal(why);
    assert.equal(r.waitMs, 60_000);
    assert.match(r.message, /removed you/);
  }
});

test("anything else is retried in three seconds, as before, with no message of its own", () => {
  assert.deepEqual(refusal({ reason: "" }), { waitMs: 3000, message: null });
  assert.deepEqual(refusal({ code: "room_full" }), { waitMs: 3000, message: null });
  assert.deepEqual(refusal({}), { waitMs: 3000, message: null });
});
