// What the client does when a room turns it away: the server closed its socket with a reason (1008 and the
// operator's reason), or the platform refused the join with a code. An operator's close is followed by the room
// opening again fresh within a minute or two; an operator's removal holds for as long as that room lives.

const CLOSED = { waitMs: 30_000, message: "This room was closed by an operator. It opens again fresh in a minute or two." };
const REMOVED = { waitMs: 60_000, message: "An operator removed you from this room. You can still walk into the other regions." };

export function refusal({ reason, code } = {}) {
  if (reason === "room_closed_by_operator" || code === "room_closed") return CLOSED;
  if (reason === "removed_by_operator" || code === "removed_from_room") return REMOVED;
  return { waitMs: 3000, message: null };
}

// The room that serves a region, from the live regions a server last sent (region numbers come as strings).
export function roomOf(regions, region) {
  const r = regions.find(r => Number(r.region) === region);
  return r ? { room: r.room, slug: r.slug } : null;
}

// The regions no live server holds, of the `count` in the world: every server knows the ground of the whole
// world, so these are drawn, only see-through. The server the player is on holds its own region even before
// its first list of live regions says so.
export function downRegions(regions, own, count) {
  const up = new Set(regions.map(r => Number(r.region)));
  const down = new Set();
  for (let r = 0; r < count; r++) if (r !== own && !up.has(r)) down.add(r);
  return down;
}
