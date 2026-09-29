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
