// The builder's clock. A real build runs on the wall clock; a dry run against the in-process server runs the same
// code faster: every wait and every interval is scaled, and now() reports the time the build itself would see.

let scale = 1;
const t0 = Date.now();

/** 1 runs in real time; 0.1 runs ten times as fast. Set once, before anything starts. */
export function setScale(s) { scale = s; }
export const getScale = () => scale;

/** Milliseconds as the build sees them. */
export const now = () => t0 + (Date.now() - t0) / scale;

export const sleep = ms => new Promise(r => setTimeout(r, Math.max(0, ms * scale)));
export const every = (ms, fn) => setInterval(fn, Math.max(1, ms * scale));
export const after = (ms, fn) => setTimeout(fn, Math.max(0, ms * scale));

export function stamp() {
  const s = Math.floor((now() - t0) / 1000);
  return `${String(Math.floor(s / 60)).padStart(2, "0")}:${String(s % 60).padStart(2, "0")}`;
}
export const log = (...a) => console.log(stamp(), ...a);
