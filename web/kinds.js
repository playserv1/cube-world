// Block kinds as this client names them. Unreal servers wrote "Stone" (the casing of an FName) until 2026-10-02, and
// the rows they wrote then are still read by every server: a kind from the wire is taken without regard to case, and
// an inventory that lists one kind under two spellings (a refill topped "Stone" up as "stone") adds both counts up.

// A stack holds 64, as on both servers.
export const STACK_SIZE = 64;

export function kindOf(kind) {
  return typeof kind === "string" ? kind.toLowerCase() : kind;
}

export function inventoryOf(stacks) {
  const out = {};
  for (const [kind, count] of Object.entries(stacks ?? {})) {
    const k = kindOf(kind);
    out[k] = Math.min((out[k] ?? 0) + count, STACK_SIZE);
  }
  return out;
}

// Folds every block kind a server frame carries to this client's spelling, in place, and returns the frame.
export function withKinds(frame) {
  const fold = c => { if (c && typeof c.kind === "string") c.kind = c.kind.toLowerCase(); };
  if (frame.inventory) frame.inventory = inventoryOf(frame.inventory);
  if (Array.isArray(frame.hotbar)) frame.hotbar = frame.hotbar.map(kindOf);
  for (const list of [frame.world, frame.layers, frame.falls]) if (Array.isArray(list)) list.forEach(fold);
  if (Array.isArray(frame.blocks)) for (const b of frame.blocks) { fold(b); b.Drop = kindOf(b.Drop); }
  if (Array.isArray(frame.changes)) for (const change of frame.changes) fold(change.cube);
  fold(frame.cube);
  if (frame.type === "fall") fold(frame);
  return frame;
}
