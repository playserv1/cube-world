// The platform settings the browser client uses (web/config.js: the API, the public client key, the room type)
// and the room the guards keep (tools/guards/room.json).

import { readFileSync, writeFileSync } from "node:fs";
import { fileURLToPath } from "node:url";

// GUARDS_ROOM names another room file, for trying the guards out on a room of their own.
export const ROOM_FILE = process.env.GUARDS_ROOM ?? fileURLToPath(new URL("./room.json", import.meta.url));

export function loadClientConfig() {
  const path = fileURLToPath(new URL("../../web/config.js", import.meta.url));
  const window = {};
  new Function("window", readFileSync(path, "utf8"))(window);
  if (!window.CUBEWORLD?.clientKey) throw new Error("web/config.js has no CUBEWORLD.clientKey (copy web/config.example.js)");
  return window.CUBEWORLD;
}

export function loadRoom() { return JSON.parse(readFileSync(ROOM_FILE, "utf8")); }

export function saveRoom(room) { writeFileSync(ROOM_FILE, `${JSON.stringify(room, null, 2)}\n`); }
