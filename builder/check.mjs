// A check of the plan itself, in seconds, before any builder is sent: blocks are put in the order the crew puts
// them (plan.mjs rank), and for each the check asks whether a builder of its zone could place it at its turn,
// from a cell it can stand in (on the floor, on the stairs, or on scaffolding up to eight high on a free column),
// with the client's ray. Whatever fails here would be set aside on the real site too.
//
//   node builder/check.mjs [--room pink]

import { World, cellOf, key, aimPlace, eyeOfCell } from "./world.mjs";
import { housePlan, rank, ROOM_ZONES } from "./plan.mjs";

const COLORS = ["red", "blue", "green", "yellow", "purple", "pink"];
const color = process.argv.includes("--room") ? process.argv[process.argv.indexOf("--room") + 1] : "pink";
const region = COLORS.indexOf(color), rx = (region % 3) * 24, ry = Math.floor(region / 3) * 24;

export function checkPlan(rx, ry, { verbose = false } = {}) {
  const w = new World();
  w.configure({ width: 72, depth: 48, minZ: -4, maxZ: 64, regionSize: 24, trees: [], blocks: [], world: [],
    layers: [{ z: -4, kind: "bedrock" }, { z: -3, kind: "dirt" }, { z: -2, kind: "dirt" }, { z: -1, kind: "grass" }] });
  const plan = housePlan(rx, ry).filter(b => b.storey >= 0).sort((a, b) => rank(a) - rank(b));
  const planKeys = new Set(plan.map(cellOf));
  const inside = (x, y) => x >= rx && x < rx + 24 && y >= ry && y < ry + 24;
  const solid = (x, y, z) => w.solid(x, y, z);
  const roomOf = name => ROOM_ZONES.find(z => z.name === name);
  const inZone = (zone, x, y) => { const r = roomOf(zone); if (!r) return true; return x >= rx + r.i0 && x <= rx + r.i1 && y >= ry + r.j0 && y <= ry + r.j1; };

  // A stand: feet and head free, something solid under the feet, or a free column to put scaffolding up on.
  function canPlace(b) {
    for (let x = b.x - 5; x <= b.x + 5; x++) for (let y = b.y - 5; y <= b.y + 5; y++) {
      if (!inside(x, y) || !inZone(b.zone, x, y)) continue;
      for (let z = b.z - 6; z <= b.z + 3; z++) {
        if (x === b.x && y === b.y && (z === b.z || z === b.z + 1)) continue;
        if (solid(x, y, z) || solid(x, y, z + 1)) continue;
        let ground = z - 1, pillar = 0;
        while (ground > b.z - 10 && !solid(x, y, ground)) { if (planKeys.has(key(x, y, ground))) { pillar = -1; break; } ground--; pillar++; }
        if (pillar < 0 || pillar > 8) continue;
        if (aimPlace(w, eyeOfCell({ x, y, z }), b, process.env.FINE === "1")) return { x, y, z, pillar };
      }
    }
    return null;
  }

  const failed = [];
  for (const b of plan) {
    if (canPlace(b)) w.overrides.set(cellOf(b), b.kind);
    else failed.push(b);
  }
  return { plan, failed };
}

if (import.meta.url === `file://${process.argv[1]}`) {
  const t = Date.now();
  const { plan, failed } = checkPlan(rx, ry);
  const by = {};
  for (const b of failed) { const k = `${b.role} (storey ${b.storey + 1})`; (by[k] ??= []).push(`${b.x - rx},${b.y - ry},${b.z}`); }
  console.log(`${plan.length} blocks checked in ${((Date.now() - t) / 1000).toFixed(1)} s; ${failed.length} could not be placed at their turn`);
  for (const [k, list] of Object.entries(by).slice(0, 40)) console.log(`  ${k}: ${list.length}  ${list.slice(0, 8).join("  ")}`);
  process.exit(failed.length ? 1 : 0);
}
