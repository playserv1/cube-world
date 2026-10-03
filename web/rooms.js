// What the client does when a room turns it away: the server closed its socket with a reason (1008 and the
// operator's reason), or the platform refused the join with a code. An operator's close is followed by the room
// opening again fresh within a minute or two. An operator's removal holds until the room is closed: a Delete room
// lets the player back in at once, under the same room name, and /rooms/{slug}:browse names no registration time
// to see that by, so a removed player knocks again on the short delay (a 409 removed_from_room is cheap).
// The title and the message are the notice over the game (#notice in app.js); `barred` is the line shown while the
// player stands in the region of a room that holds them out (#barred).

const CLOSED = { waitMs: 30_000, title: "Room closed",
  message: "This room was closed by an operator. It opens again fresh in a minute or two.",
  barred: "This room was closed by an operator and is opening again: you can enter once it is back." };
const REMOVED = { waitMs: 3000, title: "Removed from the room",
  message: "An operator removed you from this room. You can still walk into the other regions.",
  barred: "You can't enter this room: an operator removed you from it." };

export function refusal({ reason, code } = {}) {
  if (reason === "room_closed_by_operator" || code === "room_closed") return CLOSED;
  if (reason === "removed_by_operator" || code === "removed_from_room") return REMOVED;
  return { waitMs: 3000, title: null, message: null, barred: null };
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
