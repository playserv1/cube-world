// Another player on screen walks to the last position their server sent at a steady pace, and gets there about when
// the next one is due: no dash-and-wait, whatever the frame rate. How often positions come is measured per player
// (every 100 ms from this server; about every 200 ms from another one, whose writes the platform merges while it
// stores the previous one, with pauses of seconds now and then).
export const SNAP = 5;              // blocks: a respawn or a jump across regions is shown at once
export const INTERVAL = 100;        // ms between positions, until measured
export const MIN_INTERVAL = 40;
export const MAX_INTERVAL = 500;
export const SLACK = 2;             // the walk takes a bit longer than an interval, so an uneven one rarely leaves it standing

export function createFollower(p, now) {
  return { x: p.x, y: p.y, z: p.z, yaw: p.yaw, pitch: p.pitch, to: { ...p }, interval: INTERVAL, heardAt: now, arriveAt: now, drawnAt: now };
}

/** A position came: go from where the avatar stands now to it, arriving a bit more than one interval later. */
export function hear(f, p, now) {
  const { to } = f;
  if (p.x === to.x && p.y === to.y && p.z === to.z && p.yaw === to.yaw && p.pitch === to.pitch) return;
  const gap = Math.min(now - f.heardAt, MAX_INTERVAL);
  f.interval = Math.max(MIN_INTERVAL, f.interval * 0.8 + gap * 0.2);
  f.heardAt = now;
  f.to = { ...p };
  if (Math.hypot(p.x - f.x, p.y - f.y, p.z - f.z) > SNAP) Object.assign(f, { x: p.x, y: p.y, z: p.z, yaw: p.yaw, pitch: p.pitch, arriveAt: now });
  else f.arriveAt = now + f.interval * SLACK;
}

/** One frame: cover this frame's share of the way left. */
export function follow(f, now) {
  const dt = now - f.drawnAt, left = f.arriveAt - f.drawnAt;
  f.drawnAt = now;
  const k = left <= dt ? 1 : Math.max(0, dt / left);
  f.x += (f.to.x - f.x) * k;
  f.y += (f.to.y - f.y) * k;
  f.z += (f.to.z - f.z) * k;
  f.yaw += turn(f.yaw, f.to.yaw) * k;
  f.pitch += (f.to.pitch - f.pitch) * k;
}

/** The shorter way from one angle to another, in (−π, π]. */
export function turn(from, to) {
  const d = (to - from) % (2 * Math.PI);
  return d > Math.PI ? d - 2 * Math.PI : d <= -Math.PI ? d + 2 * Math.PI : d;
}
