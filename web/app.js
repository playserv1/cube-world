import * as THREE from "three";
import { PointerLockControls } from "three/addons/controls/PointerLockControls.js";
import * as S from "./spec.js";
import { createBody, tick as physicsTick, knockback, pushAway, bodyHeight, eyeHeight } from "./physics.js";
import { buildAtlas, blockIcon } from "./textures.js";
import { buildPlayerModel, animatePlayer } from "./skin.js";
import { VoxelWorld, meshChunk, chunkMaterials, blockMesh, crackMesh, raycastBlocks, raycastPlayers, buildTreeMap, TREES } from "./voxels.js";

// The server keeps x, y on the ground and z up; the client keeps y up.
const toClient = p => ({ x: p.x, y: p.z, z: p.y });
const cfg = window.CUBEWORLD;
const OFFLINE = new URLSearchParams(location.search).has("offline") || !cfg;
const SERVER_COLORS = { red: "#ef4444", blue: "#3b82f6", green: "#22c55e", grey: "#9ca3af" };
const REGION_COLORS = ["red", "blue", "green"];
const $ = id => document.getElementById(id);

const state = { player: null, socket: null, room: null, server: null, color: "grey", region: -1, regions: [],
  regionSize: 24, hotbar: [], slot: 0, inventory: {}, switching: false, placed: false,
  health: S.MAX_HEALTH, dead: false, tick: 0, dig: null, digCooldown: 0, hurtUntil: 0, fov: S.FOV };
const world = new VoxelWorld();
const chunks = new Map();
const avatars = new Map();
const cracks = new Map();
const falling = [];
const me = createBody(36, 0, 12);

function log(text) {
  $("log").textContent = `${new Date().toLocaleTimeString()} ${text}\n` + $("log").textContent;
}

// ── platform ─────────────────────────────────────────────────────────────────────────────────────

async function api(method, path, body) {
  const headers = { "X-PlayServ-Client": cfg.clientKey, "Content-Type": "application/json" };
  if (state.player) headers.Authorization = `Bearer ${state.player.access_token}`;
  const res = await fetch(`${cfg.api}${path}`, { method, headers, body: body && JSON.stringify(body) });
  const json = await res.json().catch(() => ({}));
  if (!res.ok) throw new Error(`${method} ${path} → ${res.status} ${json.code || json.title || ""}`);
  return json;
}

async function signIn(name) {
  sessionStorage.setItem("cubeworld.name", name);
  state.player = OFFLINE ? { player_id: "offline-you", access_token: "", name } : await api("POST", "/auth/players/anon", { display_name: name });
  state.player.name = name;
  $("join").hidden = true;
  $("name").blur();
  $("me").textContent = name;
}

async function refreshServers() {
  if (OFFLINE) return [];
  const page = await api("GET", `/rooms/${cfg.slug}:browse`);
  const rooms = page.data.sort((a, b) => a.room_name.localeCompare(b.room_name));
  $("servers").innerHTML = "";
  for (const room of rooms) {
    const li = document.createElement("li");
    li.className = room.room_name === state.room ? "current" : "";
    li.style.setProperty("--c", SERVER_COLORS[room.room_name.split("-")[0]] || SERVER_COLORS.grey);
    li.innerHTML = `<span>${room.room_name} · ${room.players}/${room.capacity}</span>`;
    const button = document.createElement("button");
    button.textContent = room.room_name === state.room ? "here" : "enter";
    button.onclick = () => enter(room.room_name).catch(e => log(e.message));
    li.append(button);
    $("servers").append(li);
  }
  return rooms;
}

async function enter(roomName, teleport = true) {
  if (roomName === state.room || state.switching) return;
  state.switching = true;
  try {
    const ticket = await api("POST", `/rooms/${cfg.slug}/${roomName}:join`, {});
    const c = ticket.connect;
    const url = c ? `${c.transport === "wss" ? "wss" : "ws"}://${c.host}:${c.port}/` : `${cfg.api.replace(/^http/, "ws")}/games/${cfg.slug}`;
    const socket = new WebSocket(url);
    socket.onopen = () => socket.send(JSON.stringify({
      playerId: state.player.player_id, displayName: state.player.name,
      token: state.player.access_token, reservationToken: ticket.reservation_token,
    }));
    socket.onmessage = e => {
      const frame = JSON.parse(e.data);
      if (frame.type === "welcome" && state.socket !== socket) {
        const previous = state.socket;
        state.socket = socket;
        state.room = roomName;
        state.switching = false;
        previous?.close();
        onFrame(frame, teleport || !state.placed);
        log(`${previous ? "crossed into" : "entered"} ${roomName}`);
        refreshServers().catch(() => {});
        return;
      }
      if (state.socket === socket) onFrame(frame);
    };
    socket.onclose = e => {
      if (state.socket === socket) { log(`disconnected: ${e.reason || e.code}`); state.room = null; }
      if (state.socket !== socket) state.switching = false;
    };
  } catch (e) {
    state.switching = false;
    throw e;
  }
}

function send(message) {
  if (state.socket?.readyState === WebSocket.OPEN) state.socket.send(JSON.stringify(message));
}

function regionAt(x) { return Math.floor(x / state.regionSize); }

function roomOfRegion(region) { return state.regions.find(r => Number(r.region) === region)?.room; }

// ── frames from the server ───────────────────────────────────────────────────────────────────────

function onFrame(frame, teleport) {
  switch (frame.type) {
    case "welcome": {
      Object.assign(state, { server: frame.server, color: frame.color, region: frame.region ?? -1, regions: frame.regions ?? [],
        regionSize: frame.regionSize, hotbar: frame.hotbar, inventory: frame.inventory, health: frame.you.health, dead: false });
      world.configure({ width: frame.width, depth: frame.depth, minY: frame.minZ, maxY: frame.maxZ, layers: frame.layers, blocks: frame.blocks,
        trees: frame.trees, regionSize: frame.regionSize, regionColors: REGION_COLORS });
      for (const c of frame.world) world.set(c.x, c.z, c.y, c.kind);
      rebuild(world.allChunks());
      for (const crack of cracks.values()) scene.remove(crack);
      cracks.clear();
      if (teleport) spawn(frame.you);
      $("banner").textContent = `you are on server ${frame.color}-${frame.server}`;
      $("banner").style.borderLeft = `6px solid ${SERVER_COLORS[frame.color]}`;
      $("death").hidden = true;
      renderHotbar();
      renderHearts();
      break;
    }
    case "regions":
      state.regions = frame.regions;
      break;
    case "cube": {
      const c = frame.cube;
      rebuild(world.set(c.x, c.z, c.y, frame.op === "delete" ? null : c.kind));
      if (state.dig && state.dig.key === `${c.x},${c.z},${c.y}` && world.kindAt(c.x, c.z, c.y) === "air") { state.dig = null; state.digCooldown = S.DIG_COOLDOWN_TICKS; }
      if (frame.remote) log(`${c.kind === "air" || frame.op === "delete" ? "removed" : "placed"} on server ${c.placed_on} → arrived here`);
      break;
    }
    case "fall":
      startFall(frame);
      break;
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
      log(`${nameOf(frame.player)} died${frame.by ? ` to ${nameOf(frame.by)}` : ""}`);
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

function nameOf(id) {
  if (id === state.player?.player_id) return "you";
  return avatars.get(id)?.info?.name ?? id;
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

function spawn(at) {
  const p = toClient(at);
  Object.assign(me, { x: p.x, y: p.y, z: p.z, px: p.x, py: p.y, pz: p.z, vx: 0, vy: 0, vz: 0, onGround: false });
  state.placed = true;
  unstick();
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
      icons[kind] ??= blockIcon(atlas, kind);
      slot.innerHTML = `<img src="${icons[kind]}" alt="${kind}"><b>${count}</b>`;
      if (count === 0) slot.classList.add("empty");
    }
    slot.onclick = () => { state.slot = i; renderHotbar(); };
    $("hotbar").append(slot);
  }
  $("held").textContent = state.hotbar[state.slot] ?? "";
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
  return { model, tag, target: new THREE.Vector3(), yaw: 0, pitch: 0, info: p, hurtUntil: 0, last: new THREE.Vector3() };
}

function syncAvatars(players) {
  const seen = new Set();
  for (const p of players) {
    if (p.player_id === state.player?.player_id) continue;
    seen.add(p.player_id);
    const avatar = avatars.get(p.player_id) || avatars.set(p.player_id, makeAvatar(p)).get(p.player_id);
    const c = toClient(p);
    if (!avatar.info.seen) { avatar.model.position.set(c.x, c.y, c.z); avatar.last.copy(avatar.model.position); }
    avatar.target.set(c.x, c.y, c.z);
    avatar.yaw = p.yaw;
    avatar.pitch = p.pitch ?? 0;
    avatar.info = { ...p, seen: true };
  }
  for (const [id, avatar] of avatars) if (!seen.has(id)) { scene.remove(avatar.model); avatars.delete(id); }
  $("players").innerHTML = [...avatars.values()].map(a => a.info)
    .concat(state.player ? [{ player_id: state.player.player_id, name: `${state.player.name} (you)`, color: state.color, health: state.health }] : [])
    .map(p => `<li>${p.name}<span>${Math.ceil(p.health ?? 20)} hp</span><em style="color:${SERVER_COLORS[p.color]}">${p.color}</em></li>`).join("");
}

function avatarBoxes() {
  return [...avatars.entries()].map(([id, a]) => ({ id, x: a.model.position.x, y: a.model.position.y, z: a.model.position.z,
    height: a.info.sneaking ? S.SNEAK_HEIGHT : S.HEIGHT }));
}

// ── input ────────────────────────────────────────────────────────────────────────────────────────

const controls = new PointerLockControls(camera, renderer.domElement);
renderer.domElement.addEventListener("click", () => { if (!controls.isLocked && !state.dead) controls.lock(); });
const keys = new Set();
const mouse = { left: false };
addEventListener("keydown", e => {
  if (e.target.tagName === "INPUT") return;
  keys.add(e.code);
  const n = Number(e.key);
  if (n >= 1 && n <= 9) { state.slot = n - 1; renderHotbar(); }
  if (e.code === "Space") e.preventDefault();
});
addEventListener("keyup", e => keys.delete(e.code));
addEventListener("blur", () => keys.clear());
renderer.domElement.addEventListener("wheel", e => {
  state.slot = (state.slot + (e.deltaY > 0 ? 1 : 8)) % 9;
  renderHotbar();
});
renderer.domElement.addEventListener("mousedown", e => {
  if (!controls.isLocked || state.dead) return;
  if (e.button === 0) { mouse.left = true; attackIfAimed(); }
  if (e.button === 2) place();
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
  if (!state.placed) return;

  if (!state.dead) {
    camera.getWorldDirection(look);
    const yaw = Math.atan2(-look.x, look.z);
    const input = controls.isLocked ? {
      forward: (keys.has("KeyW") ? 1 : 0) - (keys.has("KeyS") ? 1 : 0),
      strafe: (keys.has("KeyA") ? 1 : 0) - (keys.has("KeyD") ? 1 : 0),
      jump: keys.has("Space"), sneak: keys.has("ShiftLeft") || keys.has("ShiftRight"),
      sprint: keys.has("ControlLeft") || keys.has("ControlRight"), yaw,
    } : { forward: 0, strafe: 0, jump: false, sneak: false, sprint: false, yaw };
    pushAway(me, avatarBoxes());
    physicsTick(me, input, world.isSolidForPhysics);
    digTick();

    const pitch = -Math.asin(Math.max(-1, Math.min(1, look.y)));
    const pose = `${me.x.toFixed(3)},${me.y.toFixed(3)},${me.z.toFixed(3)},${yaw.toFixed(3)},${pitch.toFixed(3)},${me.onGround},${me.sneaking},${me.sprinting}`;
    if (pose !== lastPose) {
      send({ op: "move", x: me.x, y: me.z, z: me.y, yaw, pitch, onGround: me.onGround, sneaking: me.sneaking, sprinting: me.sprinting });
      lastPose = pose;
    }
  }

  const here = roomOfRegion(regionAt(me.x));
  if (!OFFLINE && here && here !== state.room && !state.switching)
    enter(here, false).catch(e => { state.switching = false; log(e.message); });
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

  for (const avatar of avatars.values()) {
    avatar.model.position.lerp(avatar.target, 0.35);
    avatar.model.rotation.y = -avatar.yaw;
    const distance = Math.hypot(avatar.model.position.x - avatar.last.x, avatar.model.position.z - avatar.last.z);
    avatar.last.copy(avatar.model.position);
    animatePlayer(avatar.model, { distance, pitch: avatar.pitch, sneaking: !!avatar.info.sneaking, hurt: avatar.hurtUntil > now });
    avatar.tag.visible = !avatar.info.sneaking;
  }

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
    const generated = kindAt(x, y, z) === kind && !overrides.has(cube.key);
    if (generated) return;
    if (kind === generated(x, y, z)) { overrides.delete(cube.key); emit({ type: "cube", op: "delete", cube, remote: false }); }
    else { overrides.set(cube.key, kind); emit({ type: "cube", op: "upsert", cube, remote: false }); }
  };
  state.socket = {
    readyState: WebSocket.OPEN,
    send(text) {
      const m = JSON.parse(text);
      if (m.op === "dig") {
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
      } else if (m.op === "attack" && m.target === dummy.player_id) {
        dummy.health = Math.max(0, dummy.health - 1);
        emit({ type: "hurt", player: dummy.player_id, health: dummy.health, kx: 0, ky: 0, strength: 0 });
        if (dummy.health === 0) { emit({ type: "death", player: dummy.player_id, by: "offline-you" }); dummy.health = 20; }
      } else if (m.op === "respawn") {
        emit({ type: "respawn", you: { x: 36, y: 12, z: 0, health: 20 } });
      }
    },
  };
  state.room = "offline";
  emit({ type: "welcome", server: "local", color: "grey", region: 1, regions: [], you: { x: 36, y: 12, z: 0, health: 20 },
    width: 72, depth: 24, regionSize: 24, minZ: -4, maxZ: 64, layers: [{ z: -4, kind: "bedrock" }, { z: -3, kind: "dirt" }, { z: -2, kind: "dirt" }, { z: -1, kind: "grass" }], trees: TREES,
    blocks, hotbar, world: [], inventory: Object.fromEntries(hotbar.map(k => [k, 64])), tick: 0 });
  let t = 0;
  setInterval(() => {
    t += 0.1;
    // Walks along x; Minecraft's yaw -π/2 faces +x, π/2 faces -x.
    dummy.x = 39 + Math.sin(t) * 3; dummy.yaw = Math.cos(t) > 0 ? -Math.PI / 2 : Math.PI / 2; dummy.sneaking = Math.sin(t / 3) > 0.8 ? 1 : 0;
    emit({ type: "players", players: [dummy] });
  }, 100);
  log("offline: no platform, a local world with one other player");
}

// ── start ────────────────────────────────────────────────────────────────────────────────────────

camera.rotation.set(-0.3, Math.PI * 0.75, 0, "YXZ");
renderHotbar();
renderHearts();
requestAnimationFrame(frame);
$("name").value = sessionStorage.getItem("cubeworld.name") || "";
$("join").onsubmit = async e => {
  e.preventDefault();
  try {
    await signIn($("name").value.trim());
    if (OFFLINE) { enterOffline(); return; }
    const rooms = await refreshServers();
    if (rooms.length) await enter(rooms[0].room_name);
    setInterval(() => refreshServers().catch(() => {}), 5000);
  } catch (err) {
    log(err.message);
  }
};
window.cubeworld = { state, enter, send, world, avatars, me, camera, aim, controls, keys, mouse };
