// node --test web/
import { test } from "node:test";
import assert from "node:assert/strict";
import { refusal, roomOf, downRegions } from "./rooms.js";

test("a room the operator closed is tried again in half a minute, when it has opened fresh", () => {
  for (const why of [{ reason: "room_closed_by_operator" }, { code: "room_closed" }]) {
    const r = refusal(why);
    assert.equal(r.waitMs, 30_000);
    assert.equal(r.title, "Room closed");
    assert.match(r.message, /closed by an operator/);
  }
});

test("a player the operator removed is told so, and knocks again on the short delay: a Delete room lets them back at once", () => {
  // /rooms/{slug}:browse names no registration time, and the room comes back under the same name within seconds,
  // so the client cannot see that the ban is gone; the platform's 409 removed_from_room is cheap to ask again.
  for (const why of [{ reason: "removed_by_operator" }, { code: "removed_from_room" }]) {
    const r = refusal(why);
    assert.equal(r.waitMs, 3000);
    assert.equal(r.title, "Removed from the room");
    assert.match(r.message, /removed you/);
  }
});

test("a room that holds the player out says so for as long as they stand in its region", () => {
  assert.match(refusal({ code: "removed_from_room" }).barred, /removed you from it/);
  assert.match(refusal({ code: "room_closed" }).barred, /closed/);
  assert.equal(refusal({ code: "room_full" }).barred, null);
});

test("anything else is retried in three seconds, as before, with no message of its own", () => {
  assert.deepEqual(refusal({ reason: "" }), { waitMs: 3000, title: null, message: null, barred: null });
  assert.deepEqual(refusal({ code: "room_full" }), { waitMs: 3000, title: null, message: null, barred: null });
  assert.deepEqual(refusal({}), { waitMs: 3000, title: null, message: null, barred: null });
});

test("a region no live server holds is down; the player's own server's region never is", () => {
  const regions = [{ region: "0", room: "red-a" }, { region: "4", room: "purple-b" }];
  assert.deepEqual([...downRegions(regions, 0, 6)], [1, 2, 3, 5]);
  assert.deepEqual([...downRegions([], 2, 6)], [0, 1, 3, 4, 5]);
  assert.deepEqual([...downRegions(regions, -1, 3)], [1, 2]);
});

test("a player standing on a down region finds its room as soon as a live list names it", () => {
  const before = [{ region: "1", room: "blue-a", slug: "cubeworld" }];
  assert.equal(roomOf(before, 4), null);
  const after = [...before, { region: "4", room: "purple-b", slug: "cubeworld-ue" }];
  assert.deepEqual(roomOf(after, 4), { room: "purple-b", slug: "cubeworld-ue" });
  assert.equal(downRegions(after, 1, 6).has(4), false);
});
