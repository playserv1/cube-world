// node --test web/
import { test } from "node:test";
import assert from "node:assert/strict";
import { createFollower, hear, follow, turn, SNAP } from "./follow.js";

const at = (x, yaw = 0) => ({ x, y: 0, z: 0, yaw, pitch: 0 });

// A player running at 5.6 blocks/s whose positions come every 100 ms, drawn at the given frame rate.
function run(fps, ms) {
  const f = createFollower(at(0), 0);
  const drawn = [];
  for (let now = 0; now <= ms; now += 1000 / fps) {
    const heard = Math.floor(now / 100) * 100;
    hear(f, at(heard * 0.0056), now);
    follow(f, now);
    drawn.push({ now, x: f.x });
  }
  return drawn;
}

test("the avatar moves at the player's speed, not in dashes, at 30 and at 144 frames a second", () => {
  for (const fps of [30, 144]) {
    const drawn = run(fps, 2000).filter(d => d.now > 500);
    for (let i = 1; i < drawn.length; i++) {
      const speed = (drawn[i].x - drawn[i - 1].x) / (drawn[i].now - drawn[i - 1].now) * 1000;
      assert.ok(speed > 5.6 * 0.7 && speed < 5.6 * 1.3, `${fps} fps: ${speed.toFixed(2)} blocks/s at ${drawn[i].now.toFixed(0)} ms`);
    }
  }
});

test("uneven positions (64 to 233 ms apart, as from another server) hardly ever leave it standing, never dashing", () => {
  let seed = 1;
  const random = () => (seed = seed * 16807 % 2147483647) / 2147483647;
  const f = createFollower(at(0), 0);
  let heard = 0, before = 0;
  const speeds = [];
  for (let now = 0; now < 20000; now += 1000 / 120) {
    if (now >= heard) { hear(f, at(now * 0.0056), now); heard = now + 64 + random() * 169; }
    follow(f, now);
    if (now > 3000) speeds.push((f.x - before) * 120);
    before = f.x;
  }
  speeds.sort((a, b) => a - b);
  const share = q => speeds[Math.floor(q * (speeds.length - 1))] / 5.6;
  const stood = speeds.filter(v => v < 0.5).length;
  assert.ok(stood < speeds.length * 0.005, `stood ${stood} frames of ${speeds.length}`);
  assert.ok(share(1) < 1.5, `up to ${share(1).toFixed(2)} of the speed`);
  assert.ok(share(0.01) > 0.7 && share(0.99) < 1.3, `p1 ${share(0.01).toFixed(2)}, p99 ${share(0.99).toFixed(2)} of the speed`);
});

test("it stops where the player stopped", () => {
  const f = createFollower(at(0), 0);
  hear(f, at(1), 100);
  follow(f, 150);
  assert.ok(f.x > 0 && f.x < 1);
  follow(f, 1000);
  assert.equal(f.x, 1);
});

test("it turns the short way across ±π", () => {
  assert.ok(Math.abs(turn(3.0, -3.0) - (2 * Math.PI - 6)) < 1e-9);
  assert.ok(Math.abs(turn(-3.0, 3.0) + (2 * Math.PI - 6)) < 1e-9);
  const f = createFollower(at(0, 3.0), 0);
  hear(f, at(0, -3.0), 50);
  follow(f, 60);
  assert.ok(f.yaw > 3.0, `yaw ${f.yaw}`);
});

test("a respawn far away is shown at once", () => {
  const f = createFollower(at(0), 0);
  hear(f, at(SNAP + 1), 100);
  follow(f, 101);
  assert.equal(f.x, SNAP + 1);
});

test("a player standing still stays still", () => {
  const f = createFollower(at(3), 0);
  for (let t = 0; t < 1000; t += 16) { hear(f, at(3), t); follow(f, t); }
  assert.equal(f.x, 3);
});
