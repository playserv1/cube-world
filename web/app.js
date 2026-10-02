import * as THREE from "three";
import { PointerLockControls } from "three/addons/controls/PointerLockControls.js";
import * as S from "./spec.js";
import { createBody, tick as physicsTick, knockback, pushAway, bodyHeight, eyeHeight } from "./physics.js";
import { buildAtlas, blockIcon } from "./textures.js";
import { buildPlayerModel, animatePlayer, paintSkin } from "./skin.js";
import { VoxelWorld, meshChunk, chunkMaterials, blockMesh, crackMesh, raycastBlocks, raycastPlayers, buildTreeMap, TREES } from "./voxels.js";
import { descend, fly, inPickupReach, explode, blastDamage } from "./bombs.js";
import { buildBomb, buildParachute, animateBomb, spawnExplosion, spawnSmoke, tickEffects } from "./bombfx.js";
import { buildTombstone } from "./tombstone.js";
import { refusal, roomOf, downRegions } from "./rooms.js";
import { createFollower, hear, follow } from "./follow.js";
import { withKinds } from "./kinds.js";

// The server keeps x, y on the ground and z up; the client keeps y up.
const toClient = p => ({ x: p.x, y: p.z, z: p.y });
const toServer = p => ({ x: p.x, y: p.z, z: p.y });
const cfg = window.CUBEWORLD;
const OFFLINE = new URLSearchParams(location.search).has("offline") || !cfg;
const SERVER_COLORS = { red: "#ef4444", blue: "#3b82f6", green: "#22c55e", yellow: "#eab308", purple: "#a855f7", pink: "#ec4899", grey: "#9ca3af" };
const REGION_COLORS = ["red", "blue", "green", "yellow", "purple", "pink"];
const $ = id => document.getElementById(id);

// The room types the world's servers register under: the C# servers' (the config's) and the Unreal servers'.
const ROOM_SLUGS = cfg.slugs ?? [cfg.slug, `${cfg.slug}-ue`];

const state = { player: null, socket: null, room: null, server: null, color: "grey", region: -1, regions: [],
  regionSize: 24, hotbar: [], slot: 0, inventory: {}, switching: false, placed: false,
  health: S.MAX_HEALTH, dead: false, tick: 0, dig: null, digCooldown: 0, hurtUntil: 0, fov: S.FOV, holding: null,
  stash: [], carry: null, inventoryOpen: false,
  // A room that turned the player away is not tried again before this time (performance.now()), per room name.
  notBefore: {}, roomSlugs: {} };
const world = new VoxelWorld();
const chunks = new Map();
const avatars = new Map();
const cracks = new Map();
const falling = [];
const bombs = new Map();
const me = createBody(36, 0, 12);

// ── platform ─────────────────────────────────────────────────────────────────────────────────────

async function api(method, path, body, retried = false) {
  const headers = { "X-PlayServ-Client": cfg.clientKey, "Content-Type": "application/json" };
  if (state.player) headers.Authorization = `Bearer ${state.player.access_token}`;
  const res = await fetch(`${cfg.api}${path}`, { method, headers, body: body && JSON.stringify(body) });
  const json = await res.json().catch(() => ({}));
  // A player's access token lasts 15 minutes: on a 401 the session is refreshed once and the call repeated.
  if (res.status === 401 && !retried && state.player?.refresh_token) { await refreshSession(); return api(method, path, body, true); }
  if (!res.ok) throw Object.assign(new Error(`${method} ${path} → ${res.status} ${json.code || json.title || ""}`), { status: res.status, code: json.code });
  return json;
}

let refreshing = null;

// Swaps the refresh token for a new access token, and schedules the next swap a minute before it runs out.
function refreshSession() {
  refreshing ??= (async () => {
    try {
      const res = await fetch(`${cfg.api}/auth/players/refresh`, {
        method: "POST", headers: { "X-PlayServ-Client": cfg.clientKey, "Content-Type": "application/json" },
        body: JSON.stringify({ refresh_token: state.player.refresh_token }),
      });
      const json = await res.json().catch(() => ({}));
      if (!res.ok) {
        // A refused token is spent for good: the guest it kept is gone, the next sign-in makes a new one.
        if (res.status === 401 || res.status === 403) forgetGuest(state.player.name);
        throw new Error(`session refresh → ${res.status} ${json.code || json.title || ""}`);
      }
      Object.assign(state.player, { access_token: json.access_token, refresh_token: json.refresh_token });
      keepGuest(state.player);
      scheduleRefresh((new Date(json.expires_at) - Date.now()) / 1000);
    } finally { refreshing = null; }
  })();
  return refreshing;
}

function scheduleRefresh(seconds) {
  clearTimeout(scheduleRefresh.timer);
  scheduleRefresh.timer = setTimeout(() => refreshSession().catch(() => {}), Math.max(10, (seconds || 900) - 60) * 1000);
}

// A guest is kept per name in this browser: signing in again under the same name resumes the same player, instead of
// adding one more to the project's players every time. The refresh token is the guest's only credential, and it is
// single-use: every rotation is written back at once. The Unreal client keeps its guests the same way, in a file.
const guestKey = name => `cubeworld.guest.${name.toLowerCase()}`;

function keepGuest(player) {
  try { localStorage.setItem(guestKey(player.name), JSON.stringify({ player_id: player.player_id, refresh_token: player.refresh_token })); } catch {}
}

function forgetGuest(name) {
  try { localStorage.removeItem(guestKey(name)); } catch {}
}

/** The guest kept under this name, signed in again; null when there is none or the platform refused its token. */
async function resumeGuest(name) {
  let kept = null;
  try { kept = JSON.parse(localStorage.getItem(guestKey(name)) || "null"); } catch {}
  if (!kept?.refresh_token || !kept.player_id) return null;
  const res = await fetch(`${cfg.api}/auth/players/refresh`, {
    method: "POST", headers: { "X-PlayServ-Client": cfg.clientKey, "Content-Type": "application/json" },
    body: JSON.stringify({ refresh_token: kept.refresh_token }),
  });
  const json = await res.json().catch(() => ({}));
  if (!res.ok) {
    if (res.status === 401 || res.status === 403) forgetGuest(name);
    else throw new Error(`session refresh → ${res.status} ${json.code || json.title || ""}`);
    return null;
  }
  const expiresIn = json.expires_at ? (new Date(json.expires_at) - Date.now()) / 1000 : json.expires_in;
  return { player_id: json.player_id ?? kept.player_id, access_token: json.access_token, refresh_token: json.refresh_token, expires_in: expiresIn };
}

async function signIn(name) {
  sessionStorage.setItem("cubeworld.name", name);
  state.player = OFFLINE ? { player_id: "offline-you", access_token: "", name }
    : await resumeGuest(name) ?? await api("POST", "/auth/players/anon", { display_name: name });
  state.player.name = name;
  if (!OFFLINE) { keepGuest(state.player); scheduleRefresh(state.player.expires_in); }
  $("join").hidden = true;
  $("name").blur();
  $("me").textContent = name;
}

async function refreshServers() {
  if (OFFLINE) return [];
  // The C# servers and the Unreal servers register under their own room types; both are listed. A type the
  // project has not got is simply missing.
  const pages = await Promise.all(ROOM_SLUGS.map(slug => api("GET", `/rooms/${slug}:browse`)
    .then(page => page.data.map(room => ({ ...room, slug })), e => { if (e.status === 404) return []; throw e; })));
  const rooms = pages.flat().sort((a, b) => a.room_name.localeCompare(b.room_name));
  $("servers").innerHTML = "";
  for (const room of rooms) {
    state.roomSlugs[room.room_name] = room.slug;
    const li = document.createElement("li");
    li.className = room.room_name === state.room ? "current" : "";
    li.style.setProperty("--c", SERVER_COLORS[room.room_name.split("-")[0]] || SERVER_COLORS.grey);
    li.innerHTML = `<span>${room.room_name} · ${room.players}/${room.capacity}</span>`;
    const button = document.createElement("button");
    button.textContent = room.room_name === state.room ? "Here" : "Enter";
    button.onclick = () => enter(room.room_name).catch(() => {});
    li.append(button);
    $("servers").append(li);
  }
  if (rooms.length === 0) {
    const li = document.createElement("li");
    li.className = "none";
    li.textContent = "No server is running";
    $("servers").append(li);
  }
  return rooms;
}

async function enter(roomName, teleport = true) {
  if (roomName === state.room || state.switching) return;
  state.switching = true;
  if (teleport) curtain(true, `Joining ${roomName}...`);
  try {
    // A region's claim can carry a stale room type (a C# server that writes none keeps the Unreal one that held the
    // region before): a room not found under one type is tried under the other.
    let slug = state.roomSlugs[roomName] ?? cfg.slug;
    let ticket;
    try { ticket = await api("POST", `/rooms/${slug}/${roomName}:join`, {}); }
    catch (e) {
      if (e.status !== 404) throw e;
      slug = ROOM_SLUGS.find(s => s !== slug) ?? slug;
      ticket = await api("POST", `/rooms/${slug}/${roomName}:join`, {});
    }
    state.roomSlugs[roomName] = slug;
    const c = ticket.connect;
    // An Unreal server plays Unreal clients on its own port and browsers on a WebSocket one, named in its attributes.
    const door = ticket.attributes && ticket.attributes.ws;
    const url = door ? door : c ? `${c.transport === "wss" ? "wss" : "ws"}://${c.host}:${c.port}/` : `${cfg.api.replace(/^http/, "ws")}/games/${slug}`;
    const socket = new WebSocket(url);
    // A server that has not welcomed the player in 10 s is given up, as the Unreal client gives up a handshake
    // (CubeGameEngine.h): the player plays on where they are, and the crossing is tried again.
    const giveUp = setTimeout(() => { if (state.socket !== socket) socket.close(); }, 10000);
    socket.onopen = () => socket.send(JSON.stringify({
      playerId: state.player.player_id, displayName: state.player.name,
      token: state.player.access_token, reservationToken: ticket.reservation_token,
    }));
    socket.onmessage = e => {
      const frame = JSON.parse(e.data);
      if (frame.type === "welcome" && state.socket !== socket) {
        clearTimeout(giveUp);
        const previous = state.socket;
        state.socket = socket;
        state.room = roomName;
        state.switching = false;
        previous?.close();
        onFrame(frame, teleport || !state.placed);
        // The new server hears where the player stands with the next tick's move, even if they stand still.
        lastPose = "";
        refreshServers().catch(() => {});
        return;
      }
      if (state.socket === socket) onFrame(frame);
    };
    socket.onclose = e => {
      clearTimeout(giveUp);
      if (state.socket === socket) {
        const turned = refusal({ reason: e.reason });
        if (turned.message) state.notBefore[roomName] = performance.now() + turned.waitMs;
        state.room = null;
      }
      if (state.socket !== socket) { state.switching = false; if (teleport) curtain(false); }
    };
  } catch (e) {
    state.switching = false;
    if (teleport) curtain(false);
    throw e;
  }
}

function send(message) {
  if (state.socket?.readyState === WebSocket.OPEN) state.socket.send(JSON.stringify(message));
}

function regionAt(x, z) { return world.regionOf(x, z); }

function roomOfRegion(region) {
  const r = roomOf(state.regions, region);
  if (r?.slug && !state.roomSlugs[r.room]) state.roomSlugs[r.room] = r.slug;   // a browse's type wins over a claim's
  return r?.room;
}

// The ground of a region no live server holds is drawn see-through; it turns solid again once its room is up.
// Offline, the one local server plays the whole world.
function downNow() { return OFFLINE ? new Set() : downRegions(state.regions, state.region, world.regionCount()); }

// ── frames from the server ───────────────────────────────────────────────────────────────────────

function onFrame(frame, teleport) {
  // Rows an Unreal server wrote before 2026-10-02 spell stone "Stone": every kind is taken in this client's spelling.
  withKinds(frame);
  switch (frame.type) {
    case "welcome": {
      Object.assign(state, { server: frame.server, color: frame.color, region: frame.region ?? -1, regions: frame.regions ?? [],
        regionSize: frame.regionSize, hotbar: [...frame.hotbar], inventory: frame.inventory, health: frame.you.health, dead: false,
        // Each server numbers its own corrections from 0.
        moveSeq: 0 });
      loadLayout(frame.hotbar);
      const before = chunks.size > 0 ? world.snapshot() : null;
      world.configure({ width: frame.width, depth: frame.depth, minY: frame.minZ, maxY: frame.maxZ, layers: frame.layers, blocks: frame.blocks,
        trees: frame.trees, regionSize: frame.regionSize, regionColors: REGION_COLORS });
      for (const c of frame.world) world.set(c.x, c.z, c.y, c.kind);
      const down = world.setDown(downNow());
      rebuild(down.length ? down : world.changedSince(before));
      for (const crack of cracks.values()) scene.remove(crack);
      cracks.clear();
      if (teleport) spawn(frame.you);
      // The old server's dig ended with the player; one still held starts again on this server with the next tick.
      state.dig = null;
      for (const id of [...bombs.keys()]) removeBomb(id);
      for (const bomb of frame.bombs ?? []) onBomb(bomb);
      // This client can show a bomb in the hand and throw it, and reads blocks batched in one "cubes" frame; the
      // server hands bombs, and batches, only to clients that say so.
      send({ op: "bombs" });
      $("banner").textContent = `you are on server ${frame.color}-${frame.server}`;
      $("banner").style.borderLeft = `6px solid ${SERVER_COLORS[frame.color]}`;
      $("death").hidden = true;
      renderHotbar();
      renderHearts();
      break;
    }
    case "regions":
      state.regions = frame.regions;
      rebuild(world.setDown(downNow()));
      break;
    case "cube": {
      const c = frame.cube;
      rebuild(world.set(c.x, c.z, c.y, frame.op === "delete" ? null : c.kind));
      if (state.dig && state.dig.key === `${c.x},${c.z},${c.y}` && world.kindAt(c.x, c.z, c.y) === "air") { state.dig = null; state.digCooldown = S.DIG_COOLDOWN_TICKS; }
      break;
    }
    case "fall":
      startFall(frame);
      break;
    case "cubes": {
      // Blocks that changed together (a blast is a hundred of them) arrive in one frame and rebuild each chunk once.
      for (const f of frame.falls) startFall(f);
      const ids = new Set();
      for (const { op, cube: c } of frame.changes) {
        for (const id of world.set(c.x, c.z, c.y, op === "delete" ? null : c.kind)) ids.add(id);
        if (state.dig && state.dig.key === `${c.x},${c.z},${c.y}` && world.kindAt(c.x, c.z, c.y) === "air") { state.dig = null; state.digCooldown = S.DIG_COOLDOWN_TICKS; }
      }
      rebuild(ids);
      break;
    }
    case "dig":
      showCrack(frame);
      break;
    case "inventory":
    case "refused":
      state.inventory = frame.inventory;
      renderHotbar();
      break;
    case "players":
      syncAvatars(frame.players);
      break;
    case "hurt":
      onHurt(frame);
      break;
    case "death":
      if (frame.player === state.player?.player_id) { state.dead = true; state.health = 0; renderHearts(); $("death").hidden = false; controls.unlock(); }
      break;
    case "bomb":
      onBomb(frame);
      break;
    case "correct":
      // A move too far for the time it took (the server's move check): back to where the last good move left us.
      state.moveSeq = frame.seq;
      snapTo(frame);
      break;
    case "respawn":
      state.dead = false;
      state.health = frame.you.health;
      $("death").hidden = true;
      spawn(frame.you);
      renderHearts();
      break;
  }
}

function onHurt(frame) {
  if (frame.player === state.player?.player_id) {
    state.health = frame.health;
    state.hurtUntil = performance.now() + S.HURT_TICKS * S.TICK_MS;
    if (frame.strength) knockback(me, frame.kx, frame.ky, frame.strength);
    renderHearts();
    return;
  }
  const avatar = avatars.get(frame.player);
  if (avatar) avatar.hurtUntil = performance.now() + S.HURT_TICKS * S.TICK_MS;
}

// From Play, or a jump from the server list, until the player stands where the server put them the view is curtained,
// then it fades in a moment later, once the world is drawn there: it never shows from the wrong place first.
let curtainTimer = 0;
function curtain(on, text) {
  const c = $("curtain");
  clearTimeout(curtainTimer);
  if (text !== undefined) $("curtain-status").textContent = text;
  c.classList.toggle("lifting", !on);
  if (on) c.classList.add("shown");
  else curtainTimer = setTimeout(() => c.classList.remove("shown"), 250);
}

function spawn(at) {
  const p = toClient(at);
  Object.assign(me, { x: p.x, y: p.y, z: p.z, px: p.x, py: p.y, pz: p.z, vx: 0, vy: 0, vz: 0, peak: p.y, onGround: false });
  state.placed = true;
  curtain(false);
  unstick();
}

function snapTo(at) {
  const p = toClient(at);
  Object.assign(me, { x: p.x, y: p.y, z: p.z, px: p.x, py: p.y, pz: p.z, vx: 0, vy: 0, vz: 0 });
  lastPose = "";
}

function unstick() {
  let guard = 0;
  while (overlapsBlocks() && guard++ < 80) { me.y = Math.floor(me.y) + 1; me.py = me.y; }
}

function overlapsBlocks() {
  const half = S.WIDTH / 2, h = bodyHeight(me);
  for (let x = Math.floor(me.x - half); x <= Math.floor(me.x + half - 1e-7); x++)
    for (let z = Math.floor(me.z - half); z <= Math.floor(me.z + half - 1e-7); z++)
      for (let y = Math.floor(me.y + 1e-7); y <= Math.floor(me.y + h - 1e-7); y++)
        if (world.isSolid(x, y, z)) return true;
  return false;
}

// ── bombs ────────────────────────────────────────────────────────────────────────────────────────
// A free bomb comes down under its parachute, a held one sits in its holder's hand, a thrown one flies the
// path the server flies it. The server says when one is picked up, thrown, explodes or fizzles out.

const BOMB_RANK = { free: 0, held: 1, flying: 2 };

function onBomb({ bomb: b, age = 0, z }) {
  const at = toClient(b);
  let e = bombs.get(b.bomb_id);
  if (b.state === "exploded") {
    spawnExplosion(scene, new THREE.Vector3(at.x, at.y, at.z));
    removeBomb(b.bomb_id);
    return;
  }
  if (b.state === "fizzled") {
    if (e || age < 5000) spawnSmoke(scene, e ? e.mesh.getWorldPosition(new THREE.Vector3()) : new THREE.Vector3(at.x, z ?? at.y, at.z));
    removeBomb(b.bomb_id);
    return;
  }
  // A bomb only moves forward (free, held, flying): a frame that would take it back is stale, and never takes a bomb
  // out of the hand (the Unreal client keeps the same rule).
  if (e && BOMB_RANK[b.state] < BOMB_RANK[e.state]) return;
  if (!e) {
    e = { id: b.bomb_id, mesh: buildBomb(), parachute: buildParachute(), pos: new THREE.Vector3(), prev: new THREE.Vector3(), vel: new THREE.Vector3(), landed: false };
    e.mesh.add(e.parachute);
    bombs.set(b.bomb_id, e);
  }
  e.state = b.state;
  e.holder = b.holder;
  e.mesh.removeFromParent();
  e.mesh.position.set(0, 0, 0);
  e.mesh.scale.setScalar(1);
  e.parachute.visible = false;
  if (b.state === "free") {
    e.pos.set(at.x, z ?? at.y, at.z);
    e.landed = false;
    scene.add(e.mesh);
  } else if (b.state === "flying") {
    e.pos.set(at.x, at.y, at.z);
    e.vel.set(b.vx, b.vz, b.vy);
    e.stopped = 0;
    for (let t = 0; t < Math.min(200, Math.floor(age / S.TICK_MS)) && !e.stopped; t++) tickBomb(e);
    scene.add(e.mesh);
  }
  e.prev.copy(e.pos);
  if (b.state !== "held") e.mesh.position.copy(e.pos);
  const mine = [...bombs.values()].find(x => x.state === "held" && x.holder === state.player?.player_id);
  state.holding = mine?.id ?? null;
  renderHotbar();
}

function removeBomb(id) {
  const e = bombs.get(id);
  if (!e) return;
  e.mesh.removeFromParent();
  bombs.delete(id);
  if (state.holding === id) { state.holding = null; renderHotbar(); }
}

function tickBomb(e) {
  e.prev.copy(e.pos);
  if (e.state === "free") {
    e.pos.y = descend(world.isSolidForPhysics, e.pos.x, e.pos.y, e.pos.z);
    e.landed = e.pos.y === e.prev.y;
  } else if (e.state === "flying" && !e.stopped) {
    if (fly(world.isSolid.bind(world), bounds(), e.pos, e.vel) !== "flying") e.stopped = state.tick;
  } else if (e.state === "flying" && state.tick - e.stopped > 60) {
    removeBomb(e.id);
  }
}

function bounds() { return { width: world.width, depth: world.depth, minY: world.minY }; }

// Where a held bomb goes: in front of the camera for the holder, in the right hand of anyone else's model.
function handOf(e) {
  if (e.holder === state.player?.player_id) return camera;
  return avatars.get(e.holder)?.model.userData.parts.rightArm ?? null;
}

function placeHeld(e) {
  const hand = handOf(e);
  if (e.mesh.parent === hand) return;
  e.mesh.removeFromParent();
  if (!hand) return;
  hand.add(e.mesh);
  const own = hand === camera;
  e.mesh.scale.setScalar(own ? 0.32 : 1);
  if (own) e.mesh.position.set(0.22, -0.22, -0.5);
  else e.mesh.position.set(0, -0.95, 0.02);
}

function throwBomb() {
  camera.getWorldDirection(look);
  send({ op: "throw", x: look.x, y: look.z, z: look.y });
}

// ── HUD ──────────────────────────────────────────────────────────────────────────────────────────

const icons = {};

function renderHotbar() {
  $("hotbar").innerHTML = "";
  for (let i = 0; i < 9; i++) {
    const kind = state.hotbar[i];
    const count = kind ? state.inventory[kind] ?? 0 : 0;
    const slot = document.createElement("div");
    slot.className = `slot${i === state.slot ? " selected" : ""}`;
    slot.title = kind ?? "";
    if (kind) {
      // A kind this client cannot draw gets a slot without an icon, rather than stopping the hotbar at it.
      icons[kind] ??= blockIcon(atlas, kind);
      slot.innerHTML = `${icons[kind] ? `<img src="${icons[kind]}" alt="${kind}">` : ""}<b>${count}</b>`;
      if (count === 0) slot.classList.add("empty");
    }
    slot.onclick = () => { state.slot = i; renderHotbar(); };
    $("hotbar").append(slot);
  }
  $("held").textContent = state.holding ? "bomb · right click throws it" : state.hotbar[state.slot] ?? "";
  $("hud").classList.toggle("holding", !!state.holding);
}

function renderHearts() {
  const hp = Math.ceil(state.health);
  $("hearts").innerHTML = Array.from({ length: 10 }, (_, i) =>
    `<i class="${hp >= 2 * i + 2 ? "full" : hp >= 2 * i + 1 ? "half" : ""}"></i>`).join("");
}

// ── scene ────────────────────────────────────────────────────────────────────────────────────────

const renderer = new THREE.WebGLRenderer({ antialias: false });
renderer.setPixelRatio(devicePixelRatio);
$("view").append(renderer.domElement);
const scene = new THREE.Scene();
scene.background = new THREE.Color("#78a7ff");
scene.fog = new THREE.Fog("#78a7ff", 40, 90);
const camera = new THREE.PerspectiveCamera(S.FOV, 1, 0.05, 300);
scene.add(camera);
scene.add(new THREE.HemisphereLight("#ffffff", "#8d8d8d", 1.6));
const sun = new THREE.DirectionalLight("#ffffff", 1.2);
sun.position.set(20, 40, 10);
scene.add(sun);

const atlas = buildAtlas();
const materials = chunkMaterials(atlas);

function rebuild(ids) {
  for (const id of ids) {
    const old = chunks.get(id);
    if (old) { scene.remove(old); old.traverse(o => o.geometry?.dispose()); }
    const mesh = meshChunk(world, atlas, materials, id);
    chunks.set(id, mesh);
    scene.add(mesh);
  }
}

// Falling sand: the block is hidden where it will land and a loose block drops there at 0.04 a tick.
function startFall({ kind, x, y, fromZ, toZ }) {
  const mesh = blockMesh(atlas, materials, kind);
  mesh.position.set(x + 0.5, fromZ + 0.5, y + 0.5);
  scene.add(mesh);
  rebuild(world.hide(x, toZ, y, true));
  falling.push({ mesh, x, z: y, y: fromZ, vy: 0, toY: toZ });
}

function tickFalling() {
  for (let i = falling.length - 1; i >= 0; i--) {
    const f = falling[i];
    f.vy = (f.vy - 0.04) * 0.98;
    f.y += f.vy;
    if (f.y > f.toY) { f.mesh.position.y = f.y + 0.5; continue; }
    scene.remove(f.mesh);
    rebuild(world.hide(f.x, f.toY, f.z, false));
    falling.splice(i, 1);
  }
}

function showCrack(frame) {
  let crack = cracks.get(frame.player);
  if (frame.stage < 0) { if (crack) { scene.remove(crack); cracks.delete(frame.player); } return; }
  if (!crack) { crack = crackMesh(atlas); cracks.set(frame.player, crack); scene.add(crack); }
  crack.position.set(frame.x + 0.5, frame.z + 0.5, frame.y + 0.5);
  crack.userData.setStage(frame.stage);
}

const highlight = new THREE.LineSegments(new THREE.EdgesGeometry(new THREE.BoxGeometry(1.004, 1.004, 1.004)),
  new THREE.LineBasicMaterial({ color: "#000000", transparent: true, opacity: 0.4 }));
highlight.visible = false;
scene.add(highlight);

// ── other players ────────────────────────────────────────────────────────────────────────────────

function nameTag(text) {
  const canvas = document.createElement("canvas");
  canvas.width = 256; canvas.height = 64;
  const g = canvas.getContext("2d");
  g.font = "bold 28px system-ui";
  const w = g.measureText(text).width + 24;
  g.fillStyle = "rgba(0,0,0,.35)"; g.fillRect(128 - w / 2, 8, w, 48);
  g.fillStyle = "#fff"; g.textAlign = "center"; g.fillText(text, 128, 42);
  const sprite = new THREE.Sprite(new THREE.SpriteMaterial({ map: new THREE.CanvasTexture(canvas), depthTest: false, transparent: true }));
  sprite.scale.set(2, 0.5, 1);
  sprite.position.y = S.HEIGHT + 0.5;
  return sprite;
}

function makeAvatar(p) {
  const model = buildPlayerModel(p.player_id);
  const tag = nameTag(p.name);
  model.add(tag);
  scene.add(model);
  const tomb = makeTomb(p.name);
  return { model, tag, tomb, walk: null, info: p, hurtUntil: 0, last: new THREE.Vector3() };
}

// A dead player leaves the map; a tombstone with their name stands where they fell until they respawn.
function makeTomb(name) {
  const tomb = buildTombstone(name);
  const tag = nameTag(name);
  tag.position.y = 1.5;
  tomb.add(tag);
  tomb.visible = false;
  scene.add(tomb);
  return tomb;
}

const isDead = info => (info.health ?? S.MAX_HEALTH) <= 0;
let myTomb = null;

function syncAvatars(players) {
  const seen = new Set();
  for (const p of players) {
    if (p.player_id === state.player?.player_id) continue;
    seen.add(p.player_id);
    const avatar = avatars.get(p.player_id) || avatars.set(p.player_id, makeAvatar(p)).get(p.player_id);
    const pose = { ...toClient(p), yaw: p.yaw, pitch: p.pitch ?? 0 }, now = performance.now();
    if (avatar.walk) hear(avatar.walk, pose, now);
    else { avatar.walk = createFollower(pose, now); avatar.model.position.set(pose.x, pose.y, pose.z); avatar.last.copy(avatar.model.position); }
    avatar.info = p;
  }
  for (const [id, avatar] of avatars) if (!seen.has(id)) { scene.remove(avatar.model, avatar.tomb); avatars.delete(id); }
  $("players").innerHTML = [...avatars.values()].map(a => a.info)
    .concat(state.player ? [{ player_id: state.player.player_id, name: `${state.player.name} (you)`, color: state.color, health: state.health }] : [])
    .map(p => `<li>${p.name}<span>${isDead(p) ? "dead" : `${Math.ceil(p.health ?? 20)} hp`}</span><em style="color:${SERVER_COLORS[p.color]}">${p.color}</em></li>`).join("");
}

function avatarBoxes() {
  return [...avatars.entries()].filter(([, a]) => !isDead(a.info)).map(([id, a]) => ({ id, x: a.model.position.x, y: a.model.position.y, z: a.model.position.z,
    height: a.info.sneaking ? S.SNEAK_HEIGHT : S.HEIGHT }));
}

// ── input ────────────────────────────────────────────────────────────────────────────────────────

const controls = new PointerLockControls(camera, renderer.domElement);
renderer.domElement.addEventListener("click", () => { if (!controls.isLocked && !state.dead && !state.inventoryOpen) controls.lock(); });
// The game menu (Esc), as the Unreal client's: the browser frees the mouse on Esc, and in play that opens the menu.
// Resume goes back to the game; Exit leaves it for the start page. A click beside the buttons does nothing.
controls.addEventListener("lock", () => { $("menu").hidden = true; });
controls.addEventListener("unlock", () => {
  if (!state.placed || state.dead || state.inventoryOpen) return;
  keys.clear();
  mouse.left = false;
  $("menu").hidden = false;
});
$("resume").onclick = () => controls.lock();
$("exit").onclick = () => { const s = state.socket; state.socket = null; s?.close?.(); location.reload(); };
const keys = new Set();
const mouse = { left: false };
addEventListener("keydown", e => {
  if (e.target.tagName === "INPUT") return;
  if (e.code === "KeyI" && state.placed) { toggleInventory(); return; }
  if (e.code === "Escape" && state.inventoryOpen) { closeInventory(); return; }
  if (state.inventoryOpen) return;
  keys.add(e.code);
  const n = Number(e.key);
  if (n >= 1 && n <= 9) { state.slot = n - 1; renderHotbar(); }
  if (e.code === "Space") e.preventDefault();
});

// ── the inventory screen (I), as Minecraft's: the player, 27 slots, the hotbar ────────────────────

const STASH_SLOTS = 27;

function toggleInventory() { if (state.inventoryOpen) closeInventory(); else openInventory(); }

function openInventory() {
  if (state.dead) return;
  state.inventoryOpen = true;
  keys.clear();
  mouse.left = false;
  controls.unlock();
  $("inventory").hidden = false;
  drawDoll();
  renderInventoryScreen();
}

function closeInventory() {
  if (state.carry) dropCarry();
  state.inventoryOpen = false;
  $("inventory").hidden = true;
  $("carry").hidden = true;
  if (!state.dead) controls.lock();
}

// The layout (which stack sits in which slot) is the player's own and stays in this browser.
function loadLayout(kinds) {
  state.stash = Array(STASH_SLOTS).fill(null);
  let saved = null;
  try { saved = JSON.parse(localStorage.getItem("cubeworld.layout") || "null"); } catch {}
  const placed = new Set([...(saved?.hotbar ?? []), ...(saved?.stash ?? [])].filter(Boolean));
  const same = saved && kinds.length === placed.size && kinds.every(k => placed.has(k));
  if (same) { state.hotbar = saved.hotbar.slice(0, 9); state.stash = saved.stash.slice(0, STASH_SLOTS); }
  while (state.hotbar.length < 9) state.hotbar.push(null);
  while (state.stash.length < STASH_SLOTS) state.stash.push(null);
}

function saveLayout() {
  try { localStorage.setItem("cubeworld.layout", JSON.stringify({ hotbar: state.hotbar, stash: state.stash })); } catch {}
}

// A click on a slot: pick the stack up, put the carried one down, or swap the two.
function slotClick(list, i) {
  const kind = list[i] ?? null;
  if (!state.carry) { if (!kind) return; state.carry = kind; list[i] = null; }
  else if (!kind) { list[i] = state.carry; state.carry = null; }
  else { list[i] = state.carry; state.carry = kind; }
  saveLayout();
  renderInventoryScreen();
  renderHotbar();
}

function dropCarry() {
  const lists = [state.hotbar, state.stash];
  for (const list of lists) { const free = list.indexOf(null); if (free >= 0) { list[free] = state.carry; break; } }
  state.carry = null;
  saveLayout();
  renderHotbar();
}

function renderInventoryScreen() {
  const grid = (id, list, hotbar) => {
    const el = $(id);
    el.innerHTML = "";
    list.forEach((kind, i) => {
      const count = kind ? state.inventory[kind] ?? 0 : 0;
      const slot = document.createElement("div");
      slot.className = `slot${hotbar && i === state.slot ? " selected" : ""}${kind && count === 0 ? " empty" : ""}`;
      slot.title = kind ?? "";
      if (kind) { icons[kind] ??= blockIcon(atlas, kind); slot.innerHTML = `<img src="${icons[kind]}" alt="${kind}"><b>${count}</b>`; }
      slot.onclick = () => slotClick(list, i);
      el.append(slot);
    });
  };
  grid("inv-main", state.stash, false);
  grid("inv-bar", state.hotbar, true);
  const carry = $("carry");
  carry.hidden = !state.carry;
  if (state.carry) { icons[state.carry] ??= blockIcon(atlas, state.carry); carry.src = icons[state.carry]; }
}

$("inventory").addEventListener("mousemove", e => {
  const box = $("inventory").getBoundingClientRect();
  $("carry").style.left = `${e.clientX - box.left}px`;
  $("carry").style.top = `${e.clientY - box.top}px`;
});
$("inventory").addEventListener("click", e => { if (e.target === $("inventory")) closeInventory(); });
$("inventory").addEventListener("contextmenu", e => e.preventDefault());

// The player as the inventory shows them: the front of the skin, four pixels to the skin's one.
function drawDoll() {
  const skin = paintSkin(state.player?.player_id ?? "steve");
  const g = $("doll").getContext("2d");
  g.imageSmoothingEnabled = false;
  g.clearRect(0, 0, 64, 128);
  const part = (sx, sy, sw, sh, dx, dy) => g.drawImage(skin, sx, sy, sw, sh, dx, dy, sw * 4, sh * 4);
  part(8, 8, 8, 8, 16, 0);      // head
  part(20, 20, 8, 12, 16, 32);  // body
  part(44, 20, 4, 12, 0, 32);   // right arm
  part(36, 52, 4, 12, 48, 32);  // left arm
  part(4, 20, 4, 12, 16, 80);   // right leg
  part(20, 52, 4, 12, 32, 80);  // left leg
}
addEventListener("keyup", e => keys.delete(e.code));
addEventListener("blur", () => keys.clear());
renderer.domElement.addEventListener("wheel", e => {
  state.slot = (state.slot + (e.deltaY > 0 ? 1 : 8)) % 9;
  renderHotbar();
});
renderer.domElement.addEventListener("mousedown", e => {
  if (!controls.isLocked || state.dead) return;
  if (e.button === 0) { mouse.left = true; attackIfAimed(); }
  if (e.button === 2) { if (state.holding) throwBomb(); else place(); }
});
addEventListener("mouseup", e => { if (e.button === 0) mouse.left = false; });
renderer.domElement.addEventListener("contextmenu", e => e.preventDefault());
$("respawn").onclick = () => send({ op: "respawn" });

const look = new THREE.Vector3();
const eye = new THREE.Vector3();

function aim() {
  camera.getWorldDirection(look);
  eye.set(me.x, me.y + eyeHeight(me), me.z);
  const block = raycastBlocks(world, eye, look, S.BLOCK_REACH);
  const player = raycastPlayers(eye, look, S.ENTITY_REACH, avatarBoxes());
  if (player && (!block || player.distance < block.distance)) return { player: player.player };
  return block ? { block } : null;
}

function attackIfAimed() {
  const target = aim();
  if (target?.player) send({ op: "attack", target: target.player.id });
}

function place() {
  const target = aim();
  if (!target?.block) return;
  const b = target.block, kind = state.hotbar[state.slot];
  if (!kind || !(state.inventory[kind] > 0)) return;
  send({ op: "place", x: b.x, y: b.z, z: b.y, nx: b.nx, ny: b.nz, nz: b.ny, kind });
}

function digTick() {
  if (state.digCooldown > 0) { state.digCooldown--; return; }
  const target = mouse.left && controls.isLocked && !state.dead ? aim()?.block : null;
  const key = target ? `${target.x},${target.y},${target.z}` : null;
  if (key === (state.dig?.key ?? null)) return;
  if (state.dig) send({ op: "dig", state: "stop", x: state.dig.x, y: state.dig.z, z: state.dig.y });
  state.dig = target ? { key, x: target.x, y: target.y, z: target.z } : null;
  if (target) send({ op: "dig", state: "start", x: target.x, y: target.z, z: target.y });
}

// ── the tick and the frame ───────────────────────────────────────────────────────────────────────

let lastPose = "";

function gameTick() {
  state.tick++;
  tickFalling();
  for (const e of [...bombs.values()]) tickBomb(e);
  if (!state.placed) return;

  if (!state.dead) {
    camera.getWorldDirection(look);
    const yaw = Math.atan2(-look.x, look.z);
    const input = controls.isLocked ? {
      forward: (keys.has("KeyW") ? 1 : 0) - (keys.has("KeyS") ? 1 : 0),
      strafe: (keys.has("KeyA") ? 1 : 0) - (keys.has("KeyD") ? 1 : 0),
      // Swapped from Minecraft's default on request: Shift sprints, Ctrl sneaks.
      jump: keys.has("Space"), sneak: keys.has("ControlLeft") || keys.has("ControlRight"),
      sprint: keys.has("ShiftLeft") || keys.has("ShiftRight"), yaw,
    } : { forward: 0, strafe: 0, jump: false, sneak: false, sprint: false, yaw };
    pushAway(me, avatarBoxes());
    physicsTick(me, input, world.isSolidForPhysics);
    digTick();

    const pitch = -Math.asin(Math.max(-1, Math.min(1, look.y)));
    const pose = `${me.x.toFixed(3)},${me.y.toFixed(3)},${me.z.toFixed(3)},${yaw.toFixed(3)},${pitch.toFixed(3)},${me.onGround},${me.sneaking},${me.sprinting}`;
    if (pose !== lastPose) {
      // In the air the move says the fall's highest point too: a fall that began on the old server counts on the next.
      send({ op: "move", x: me.x, y: me.z, z: me.y, yaw, pitch, onGround: me.onGround, sneaking: me.sneaking, sprinting: me.sprinting,
        seq: state.moveSeq ?? 0, ...(me.onGround ? {} : { peak: me.peak }) });
      lastPose = pose;
    }
  }

  // A crossing that fails is tried again three seconds later, not on every tick; a room an operator closed, or
  // removed the player from, waits longer (rooms.js).
  const here = roomOfRegion(regionAt(me.x, me.z));
  const now = performance.now();
  if (!OFFLINE && here && here !== state.room && !state.switching && now >= (state.crossAfter ?? 0) && now >= (state.notBefore[here] ?? 0))
    enter(here, false).catch(e => {
      state.switching = false;
      const turned = refusal({ code: e.code });
      if (turned.message) state.notBefore[here] = now + turned.waitMs; else state.crossAfter = now + 3000;
    });
}

let accumulator = 0;
const clock = new THREE.Clock();

function frame() {
  const now = performance.now();
  accumulator += Math.min(clock.getDelta() * 1000, 250);
  while (accumulator >= S.TICK_MS) { gameTick(); accumulator -= S.TICK_MS; }
  const partial = accumulator / S.TICK_MS;

  camera.position.set(me.px + (me.x - me.px) * partial, me.py + (me.y - me.py) * partial + eyeHeight(me), me.pz + (me.z - me.pz) * partial);
  const fov = S.FOV * (me.sprinting ? S.SPRINT_FOV : 1);
  if (Math.abs(state.fov - fov) > 0.01) { state.fov += (fov - state.fov) * 0.25; camera.fov = state.fov; camera.updateProjectionMatrix(); }

  const target = controls.isLocked && !state.dead ? aim() : null;
  highlight.visible = !!target?.block;
  if (target?.block) highlight.position.set(target.block.x + 0.5, target.block.y + 0.5, target.block.z + 0.5);
  $("hurt").style.opacity = state.hurtUntil > now ? "1" : "0";

  for (const e of bombs.values()) {
    if (e.state === "held") placeHeld(e);
    else e.mesh.position.lerpVectors(e.prev, e.pos, partial);
    e.parachute.visible = e.state === "free" && !e.landed;
    if (e.parachute.visible) e.parachute.rotation.z = Math.sin(now / 700 + e.pos.x) * 0.08;
    animateBomb(e.mesh, now);
  }
  tickEffects(Math.min(0.1, (now - (frame.last ?? now)) / 1000));
  frame.last = now;

  for (const avatar of avatars.values()) {
    const walk = avatar.walk;
    follow(walk, now);
    avatar.model.position.set(walk.x, walk.y, walk.z);
    avatar.model.rotation.y = -walk.yaw;
    const distance = Math.hypot(avatar.model.position.x - avatar.last.x, avatar.model.position.z - avatar.last.z);
    avatar.last.copy(avatar.model.position);
    animatePlayer(avatar.model, { distance, pitch: walk.pitch, sneaking: !!avatar.info.sneaking, hurt: avatar.hurtUntil > now });
    avatar.tag.visible = !avatar.info.sneaking;
    const dead = isDead(avatar.info);
    avatar.model.visible = !dead;
    avatar.tomb.visible = dead;
    if (dead) { avatar.tomb.position.set(walk.to.x, walk.to.y, walk.to.z); avatar.tomb.rotation.y = -walk.to.yaw; }
  }
  if (state.dead && state.player) {
    myTomb ??= makeTomb(state.player.name);
    if (!myTomb.visible) { myTomb.position.set(me.x, me.y, me.z); myTomb.rotation.y = camera.rotation.y; }
  }
  if (myTomb) myTomb.visible = state.dead;

  const w = $("view").clientWidth, h = $("view").clientHeight;
  if (renderer.domElement.width !== Math.floor(w * devicePixelRatio) || renderer.domElement.height !== Math.floor(h * devicePixelRatio)) {
    renderer.setSize(w, h);
    camera.aspect = w / h;
    camera.updateProjectionMatrix();
  }
  renderer.render(scene, camera);
  requestAnimationFrame(frame);
}

// ── offline: the client alone, for looking at the world without the platform ─────────────────────

function enterOffline() {
  const blocks = [
    { kind: "air", Hardness: 0, NeedsTool: false, Transparent: true, Gravity: false, Drop: null, breakTicks: -1 },
    { kind: "bedrock", Hardness: -1, NeedsTool: true, Transparent: false, Gravity: false, Drop: null, breakTicks: -1 },
    { kind: "grass", Hardness: 0.6, NeedsTool: false, Transparent: false, Gravity: false, Drop: "dirt", breakTicks: 18 },
    { kind: "dirt", Hardness: 0.5, NeedsTool: false, Transparent: false, Gravity: false, Drop: "dirt", breakTicks: 15 },
    { kind: "sand", Hardness: 0.5, NeedsTool: false, Transparent: false, Gravity: true, Drop: "sand", breakTicks: 15 },
    { kind: "stone", Hardness: 1.5, NeedsTool: true, Transparent: false, Gravity: false, Drop: null, breakTicks: 150 },
    { kind: "wood", Hardness: 2, NeedsTool: false, Transparent: false, Gravity: false, Drop: "wood", breakTicks: 60 },
    { kind: "brick", Hardness: 2, NeedsTool: true, Transparent: false, Gravity: false, Drop: null, breakTicks: 200 },
    { kind: "glass", Hardness: 0.3, NeedsTool: false, Transparent: true, Gravity: false, Drop: null, breakTicks: 9 },
    { kind: "gold", Hardness: 3, NeedsTool: true, Transparent: false, Gravity: false, Drop: null, breakTicks: 300 },
    { kind: "leaves", Hardness: 0.2, NeedsTool: false, Transparent: true, Gravity: false, Drop: null, breakTicks: 6 },
  ];
  const hotbar = ["grass", "dirt", "sand", "stone", "wood", "brick", "glass", "gold", "leaves"];
  const overrides = new Map();
  const trees = buildTreeMap(TREES);
  const dummy = { player_id: "offline-steve", name: "Steve", server: "local", color: "blue", x: 39, y: 12, z: 0, yaw: Math.PI, pitch: 0, health: 20, sneaking: 0, sprinting: 0 };
  let dig = null;
  const generated = (x, y, z) => z >= 0 ? trees.get(`${x},${z},${y}`) ?? "air" : z === -4 ? "bedrock" : z === -3 || z === -2 ? "dirt" : z === -1 ? "grass" : "air";
  const kindAt = (x, y, z) => overrides.get(`${x}:${y}:${z}`) ?? generated(x, y, z);
  const emit = f => onFrame(f, true);
  const setBlock = (x, y, z, kind) => {
    const cube = { key: `${x}:${y}:${z}`, x, y, z, kind, placed_by: "you", placed_on: "local" };
    const unchanged = kindAt(x, y, z) === kind && !overrides.has(cube.key);
    if (unchanged) return;
    if (kind === generated(x, y, z)) { overrides.delete(cube.key); emit({ type: "cube", op: "delete", cube, remote: false }); }
    else { overrides.set(cube.key, kind); emit({ type: "cube", op: "upsert", cube, remote: false }); }
  };
  // Bombs as the drop function and the server handle them: two dropped every minute over each room's region (here the
  // one local room's, region 1; two more to start, near the spawn), at most five free in a region, picked up by walking
  // into them, thrown with right click.
  const offBombs = new Map();
    const record = (b, p, extra = {}) => ({ bomb_id: b.id, state: b.state, holder: b.holder ?? "", ...toServer(p), vx: 0, vy: 0, vz: 0, dropped_at: b.dropped, at: Date.now(), ...extra });
  const emitBomb = (b, p, extra) => emit({ type: "bomb", bomb: record(b, p, extra), age: 0, z: p.y });
  const dropBomb = (x, z) => {
    const free = [...offBombs.values()].filter(b => b.state === "free").sort((a, b) => a.dropped - b.dropped);
    while (free.length >= 5) { const old = free.shift(); old.state = "fizzled"; offBombs.delete(old.id); emitBomb(old, old.p); }
    const b = { id: `bomb-${Date.now()}-${offBombs.size}`, state: "free", p: new THREE.Vector3(x, S.DROP_HEIGHT, z), dropped: Date.now() };
    offBombs.set(b.id, b);
    emitBomb(b, b.p);
  };
  const hurtOffline = (id, damage, dir, impact) => {
    if (id === "offline-you") {
      const health = Math.max(0, state.health - damage);
      emit({ type: "hurt", player: id, health, kx: dir.x * impact, ky: dir.z * impact, strength: impact });
      if (health === 0) emit({ type: "death", player: id, by: "offline-you" });
    } else {
      dummy.health = Math.max(0, dummy.health - damage);
      emit({ type: "hurt", player: id, health: dummy.health, kx: 0, ky: 0, strength: 0 });
      if (dummy.health === 0) { emit({ type: "death", player: id, by: "offline-you" }); setTimeout(() => { dummy.health = 20; }, 5000); }
    }
  };
  const blowUp = b => {
    const block = (x, y, z) => {
      const kind = world.inside(x, y, z) ? world.kindAt(x, y, z) : "air";
      return kind === "air" ? null : kind === "bedrock" ? { resistance: 3600000, breakable: false } : { resistance: S.CRATER_RESISTANCE, breakable: true };
    };
    for (const [x, y, z] of explode(block, b.p.x, b.p.y, b.p.z, S.CRATER_POWER)) setBlock(x, z, y, "air");
    const targets = [{ id: "offline-you", x: me.x, y: me.y, z: me.z }].concat(dummy.health > 0 ? [{ id: dummy.player_id, ...toClient(dummy) }] : []);
    for (const t of targets) {
      const d = new THREE.Vector3(t.x - b.p.x, t.y - b.p.y, t.z - b.p.z);
      const hit = blastDamage(d.length(), S.BOMB_POWER);
      if (hit) hurtOffline(t.id, hit.damage, d.setY(d.y + S.EYE_HEIGHT).normalize(), hit.impact);
    }
    b.state = "exploded";
    offBombs.delete(b.id);
    emitBomb(b, b.p);
  };
  setInterval(() => { for (let i = 0; i < 2; i++) dropBomb(25 + Math.random() * 22, 1 + Math.random() * 22); }, 60000);
  setInterval(() => {
    for (const b of [...offBombs.values()]) {
      if (b.state === "free") {
        b.p.y = descend(world.isSolidForPhysics, b.p.x, b.p.y, b.p.z);
        const holding = [...offBombs.values()].some(o => o.state === "held");
        if (!holding && !state.dead && inPickupReach(me, bodyHeight(me), b.p.x, b.p.y, b.p.z)) { b.state = "held"; b.holder = "offline-you"; emitBomb(b, b.p); }
      } else if (b.state === "flying") {
        const players = [{ id: dummy.player_id, ...toClient(dummy) }, { id: "offline-you", x: me.x, y: me.y, z: me.z }];
        const result = b.age++ > 200 ? "exploded" : fly(world.isSolid.bind(world), bounds(), b.p, b.v, players, "offline-you", b.age);
        if (result === "exploded") blowUp(b);
        else if (result === "gone") { b.state = "fizzled"; offBombs.delete(b.id); emitBomb(b, b.p); }
      }
    }
  }, S.TICK_MS);
  state.socket = {
    readyState: WebSocket.OPEN,
    send(text) {
      const m = JSON.parse(text);
      if (m.op === "throw") {
        const b = [...offBombs.values()].find(o => o.state === "held");
        if (!b) return;
        const v = new THREE.Vector3(m.x, m.z, m.y).normalize().multiplyScalar(S.THROW_SPEED);
        Object.assign(b, { state: "flying", age: 0, p: new THREE.Vector3(me.x, me.y + eyeHeight(me), me.z), v });
        emitBomb(b, b.p, { vx: v.x, vy: v.z, vz: v.y });
      } else if (m.op === "dig") {
        if (dig) { clearInterval(dig.timer); emit({ type: "dig", player: "offline-you", x: dig.x, y: dig.y, z: dig.z, stage: -1 }); dig = null; }
        if (m.state !== "start") return;
        const kind = kindAt(m.x, m.y, m.z), block = blocks.find(b => b.kind === kind);
        if (!block || block.breakTicks < 0 || kind === "air") return;
        dig = { x: m.x, y: m.y, z: m.z, start: performance.now(), ticks: block.breakTicks };
        dig.timer = setInterval(() => {
          const elapsed = (performance.now() - dig.start) / S.TICK_MS;
          if (elapsed < dig.ticks) { emit({ type: "dig", player: "offline-you", x: dig.x, y: dig.y, z: dig.z, stage: Math.min(9, Math.floor(elapsed * 10 / dig.ticks)) }); return; }
          clearInterval(dig.timer);
          emit({ type: "dig", player: "offline-you", x: dig.x, y: dig.y, z: dig.z, stage: -1 });
          const { x, y, z } = dig; dig = null;
          if (block.Drop) { state.inventory[block.Drop] = Math.min(64, (state.inventory[block.Drop] ?? 0) + 1); emit({ type: "inventory", inventory: state.inventory }); }
          setBlock(x, y, z, "air");
          let zz = z + 1;
          while (blocks.find(b => b.kind === kindAt(x, y, zz))?.Gravity) {
            let to = zz; while (to - 1 >= -4 && kindAt(x, y, to - 1) === "air") to--;
            const k = kindAt(x, y, zz);
            emit({ type: "fall", kind: k, x, y, fromZ: zz, toZ: to });
            setBlock(x, y, zz, "air"); setBlock(x, y, to, k);
            zz++;
          }
        }, S.TICK_MS);
      } else if (m.op === "place") {
        const x = m.x + m.nx, y = m.y + m.ny, z = m.z + m.nz;
        if (kindAt(x, y, z) !== "air" || z < -4 || z >= 64) return;
        state.inventory[m.kind]--; emit({ type: "inventory", inventory: state.inventory });
        const block = blocks.find(b => b.kind === m.kind);
        let to = z; if (block.Gravity) while (to - 1 >= -4 && kindAt(x, y, to - 1) === "air") to--;
        setBlock(x, y, to, m.kind);
        if (to !== z) emit({ type: "fall", kind: m.kind, x, y, fromZ: z, toZ: to });
      } else if (m.op === "attack" && m.target === dummy.player_id && dummy.health > 0) {
        dummy.health = Math.max(0, dummy.health - 1);
        emit({ type: "hurt", player: dummy.player_id, health: dummy.health, kx: 0, ky: 0, strength: 0 });
        if (dummy.health === 0) { emit({ type: "death", player: dummy.player_id, by: "offline-you" }); setTimeout(() => { dummy.health = 20; }, 5000); }
      } else if (m.op === "respawn") {
        emit({ type: "respawn", you: { x: 36, y: 12, z: 0, health: 20 } });
      }
    },
  };
  state.room = "offline";
  emit({ type: "welcome", server: "local", color: "grey", region: 1, regions: [], you: { x: 36, y: 12, z: 0, health: 20 },
    width: 72, depth: 48, regionSize: 24, minZ: -4, maxZ: 64, layers: [{ z: -4, kind: "bedrock" }, { z: -3, kind: "dirt" }, { z: -2, kind: "dirt" }, { z: -1, kind: "grass" }], trees: TREES,
    blocks, hotbar, world: [], inventory: Object.fromEntries(hotbar.map(k => [k, 64])), tick: 0 });
  dropBomb(38.5, 14.5);
  dropBomb(33.5, 9.5);
  let t = 0;
  setInterval(() => {
    t += 0.1;
    // Walks along x; Minecraft's yaw -π/2 faces +x, π/2 faces -x.
    if (dummy.health > 0) { dummy.x = 39 + Math.sin(t) * 3; dummy.yaw = Math.cos(t) > 0 ? -Math.PI / 2 : Math.PI / 2; dummy.sneaking = Math.sin(t / 3) > 0.8 ? 1 : 0; }
    emit({ type: "players", players: [dummy] });
  }, 100);
}

// ── start ────────────────────────────────────────────────────────────────────────────────────────

camera.rotation.set(-0.3, Math.PI * 0.75, 0, "YXZ");
renderHotbar();
renderHearts();
requestAnimationFrame(frame);
$("name").value = sessionStorage.getItem("cubeworld.name") || "";
$("join").onsubmit = async e => {
  e.preventDefault();
  curtain(true, "Signing in...");
  try {
    await signIn($("name").value.trim());
    if (OFFLINE) { enterOffline(); return; }
    const rooms = await refreshServers();
    if (rooms.length) await enter(rooms[0].room_name);
    else curtain(false);
    setInterval(() => refreshServers().catch(() => {}), 5000);
  } catch { curtain(false); }
};
window.cubeworld = { state, enter, send, world, avatars, me, camera, aim, controls, keys, mouse, bombs };
