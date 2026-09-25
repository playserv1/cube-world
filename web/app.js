import * as THREE from "three";
import { PointerLockControls } from "three/addons/controls/PointerLockControls.js";

const cfg = window.CUBEWORLD;
const KIND_COLORS = { grass: "#4ade80", stone: "#94a3b8", wood: "#b45309", brick: "#dc2626", glass: "#7dd3fc", gold: "#facc15" };
const SERVER_COLORS = { red: "#ef4444", blue: "#3b82f6", green: "#22c55e", amber: "#f59e0b", violet: "#8b5cf6", cyan: "#06b6d4", pink: "#ec4899", lime: "#84cc16", grey: "#9ca3af" };
const EYE = 1.6, HEIGHT = 1.8, RADIUS = 0.3, GRAVITY = 22, JUMP = 7.5, SPEED = 4.5, REACH = 6;

const $ = id => document.getElementById(id);
const REGION_COLORS = ["red", "blue", "green"];
const state = { player: null, socket: null, room: null, server: null, color: "grey", region: -1, regions: [],
  width: 72, depth: 24, regionSize: 24, height: 8, kinds: [], kind: "stone", inventory: 0, max: 10, switching: false, placed: false };
const cubes = new Map();
const avatars = new Map();

function log(text) {
  $("log").textContent = `${new Date().toLocaleTimeString()} ${text}\n` + $("log").textContent;
}

function serverColorOf(roomName) {
  return SERVER_COLORS[roomName.split("-")[0]] || SERVER_COLORS.grey;
}

function playerColor(id) {
  let h = 0;
  for (const ch of id) h = (h * 31 + ch.charCodeAt(0)) >>> 0;
  return new THREE.Color().setHSL((h % 360) / 360, 0.75, 0.55);
}

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
  state.player = await api("POST", "/auth/players/anon", { display_name: name });
  state.player.name = name;
  $("join").hidden = true;
  $("me").textContent = name;
}

async function refreshServers() {
  const page = await api("GET", `/rooms/${cfg.slug}:browse`);
  const rooms = page.data.sort((a, b) => a.room_name.localeCompare(b.room_name));
  $("servers").innerHTML = "";
  for (const room of rooms) {
    const li = document.createElement("li");
    li.className = room.room_name === state.room ? "current" : "";
    li.style.setProperty("--c", serverColorOf(room.room_name));
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
        if (teleport || !state.placed) spawn(frame.you);
        lastPose = "";
        log(`${previous ? "crossed into" : "entered"} ${roomName}`);
        refreshServers().catch(() => {});
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

function spawn(at) {
  me.pos.set(at.x, 0, at.y);
  me.vy = 0;
  state.placed = true;
  unstick();
}

function unstick() {
  while (collides(me.pos) && me.pos.y < state.height + 2) me.pos.y = Math.floor(me.pos.y) + 1;
}

function regionAt(x) {
  return Math.floor(x / state.regionSize);
}

function roomOfRegion(region) {
  return state.regions.find(r => Number(r.region) === region)?.room;
}

function send(message) {
  if (state.socket?.readyState === WebSocket.OPEN) state.socket.send(JSON.stringify(message));
}

function onFrame(frame) {
  switch (frame.type) {
    case "welcome":
      Object.assign(state, { server: frame.server, color: frame.color, region: frame.region ?? -1, regions: frame.regions ?? [],
        width: frame.width, depth: frame.depth, regionSize: frame.regionSize, height: frame.height, kinds: frame.kinds, inventory: frame.inventory });
      for (const key of [...cubes.keys()]) removeCube(key);
      frame.world.forEach(c => addCube(c, false));
      paintFloors();
      $("banner").textContent = `you are on server ${frame.color}-${frame.server}`;
      $("banner").style.borderLeft = `6px solid ${SERVER_COLORS[frame.color]}`;
      renderKinds();
      break;
    case "regions":
      state.regions = frame.regions;
      paintFloors();
      break;
    case "cube":
      if (frame.op === "delete") removeCube(frame.cube.key); else addCube(frame.cube, frame.remote);
      if (frame.remote) log(`${frame.op === "delete" ? "removed" : "placed"} on server ${frame.cube.placed_on} → arrived here`);
      break;
    case "inventory":
    case "refused":
      state.inventory = frame.inventory;
      if (frame.type === "refused" && frame.inventory === 0) log("out of cubes, wait for the refill");
      break;
    case "players":
      syncAvatars(frame.players);
      break;
  }
  renderInventory();
}

function renderKinds() {
  $("kinds").innerHTML = "";
  state.kinds.forEach(kind => {
    const b = document.createElement("button");
    b.style.background = KIND_COLORS[kind];
    b.title = kind;
    b.className = kind === state.kind ? "selected" : "";
    b.onclick = () => { state.kind = kind; renderKinds(); };
    $("kinds").append(b);
  });
}

function renderInventory() {
  $("inventory").innerHTML = Array.from({ length: state.max }, (_, i) => `<span class="${i < state.inventory ? "full" : ""}"></span>`).join("");
}

const renderer = new THREE.WebGLRenderer({ antialias: true });
renderer.setPixelRatio(devicePixelRatio);
$("view").append(renderer.domElement);
const scene = new THREE.Scene();
scene.background = new THREE.Color("#9fd3ff");
scene.fog = new THREE.Fog("#9fd3ff", 30, 70);
const camera = new THREE.PerspectiveCamera(75, 1, 0.05, 200);
scene.add(new THREE.HemisphereLight("#ffffff", "#445544", 1.1));
const sun = new THREE.DirectionalLight("#ffffff", 1.4);
sun.position.set(20, 40, 10);
scene.add(sun);

const floors = REGION_COLORS.map((_, i) => {
  const floor = new THREE.Mesh(new THREE.PlaneGeometry(1, 1), new THREE.MeshLambertMaterial({ color: "#888" }));
  floor.rotation.x = -Math.PI / 2;
  floor.userData.region = i;
  scene.add(floor);
  return floor;
});
let grid = null;

function paintFloors() {
  floors.forEach((floor, i) => {
    const live = state.regions.some(r => Number(r.region) === i);
    floor.scale.set(state.regionSize, state.depth, 1);
    floor.position.set((i + 0.5) * state.regionSize, 0, state.depth / 2);
    floor.material.color.set(SERVER_COLORS[REGION_COLORS[i]]).lerp(new THREE.Color(live ? "#ffffff" : "#333333"), live ? 0.15 : 0.6);
  });
  if (grid) scene.remove(grid);
  grid = new THREE.GridHelper(state.width, state.width, "#000000", "#000000");
  grid.scale.z = state.depth / state.width;
  grid.material.opacity = 0.18;
  grid.material.transparent = true;
  grid.position.set(state.width / 2, 0.01, state.depth / 2);
  scene.add(grid);
}

const cubeGeometry = new THREE.BoxGeometry(1, 1, 1);
const edgesGeometry = new THREE.EdgesGeometry(cubeGeometry);
const kindMaterials = Object.fromEntries(Object.entries(KIND_COLORS).map(([k, c]) =>
  [k, new THREE.MeshLambertMaterial({ color: c, transparent: k === "glass", opacity: k === "glass" ? 0.6 : 1 })]));

function addCube(c, remote) {
  removeCube(c.key);
  const mesh = new THREE.Mesh(cubeGeometry, kindMaterials[c.kind] || kindMaterials.stone);
  mesh.position.set(c.x + 0.5, c.z + 0.5, c.y + 0.5);
  mesh.userData = c;
  if (remote) {
    const outline = new THREE.LineSegments(edgesGeometry, new THREE.LineBasicMaterial({ color: "#ffffff", transparent: true }));
    outline.scale.setScalar(1.04);
    outline.userData.born = performance.now();
    mesh.add(outline);
  }
  scene.add(mesh);
  cubes.set(c.key, mesh);
}

function removeCube(key) {
  const mesh = cubes.get(key);
  if (mesh) { scene.remove(mesh); cubes.delete(key); }
}

function solid(x, y, z) {
  if (y < 0) return true;
  if (x < 0 || z < 0 || x >= state.width || z >= state.depth) return true;
  return cubes.has(`${x}:${z}:${y}`);
}

function nameTag(text, color) {
  const canvas = document.createElement("canvas");
  canvas.width = 256; canvas.height = 64;
  const g = canvas.getContext("2d");
  g.fillStyle = "rgba(0,0,0,.55)"; g.fillRect(0, 0, 256, 64);
  g.fillStyle = color; g.fillRect(0, 0, 10, 64);
  g.fillStyle = "#fff"; g.font = "bold 26px system-ui"; g.fillText(text, 20, 42);
  const sprite = new THREE.Sprite(new THREE.SpriteMaterial({ map: new THREE.CanvasTexture(canvas), depthTest: false }));
  sprite.scale.set(1.6, 0.4, 1);
  sprite.position.y = 2.2;
  return sprite;
}

function makeAvatar(p) {
  const color = playerColor(p.player_id);
  const body = new THREE.Group();
  const torso = new THREE.Mesh(new THREE.BoxGeometry(0.6, 1.1, 0.35), new THREE.MeshLambertMaterial({ color }));
  torso.position.y = 0.95;
  const head = new THREE.Mesh(new THREE.BoxGeometry(0.45, 0.45, 0.45), new THREE.MeshLambertMaterial({ color: color.clone().offsetHSL(0, 0, 0.2) }));
  head.position.y = 1.75;
  const legs = new THREE.Mesh(new THREE.BoxGeometry(0.5, 0.45, 0.3), new THREE.MeshLambertMaterial({ color: "#1f2937" }));
  legs.position.y = 0.22;
  body.add(torso, head, legs, nameTag(`${p.name} · ${p.color}`, SERVER_COLORS[p.color] || SERVER_COLORS.grey));
  body.userData = { target: new THREE.Vector3(), yaw: 0 };
  scene.add(body);
  return body;
}

function syncAvatars(players) {
  const seen = new Set();
  for (const p of players) {
    if (p.player_id === state.player?.player_id) continue;
    seen.add(p.player_id);
    const avatar = avatars.get(p.player_id) || avatars.set(p.player_id, makeAvatar(p)).get(p.player_id);
    avatar.userData.target.set(p.x, p.z, p.y);
    avatar.userData.yaw = p.yaw;
    avatar.userData.info = p;
  }
  for (const [id, avatar] of avatars) if (!seen.has(id)) { scene.remove(avatar); avatars.delete(id); }
  $("players").innerHTML = [...avatars.values()].map(a => a.userData.info)
    .concat(state.player ? [{ player_id: state.player.player_id, name: `${state.player.name} (you)`, color: state.color }] : [])
    .map(p => `<li><i style="background:#${playerColor(p.player_id).getHexString()}"></i>${p.name}<em style="color:${SERVER_COLORS[p.color]}">${p.color}</em></li>`).join("");
}

const controls = new PointerLockControls(camera, renderer.domElement);
renderer.domElement.addEventListener("click", () => { if (!controls.isLocked) controls.lock(); });
const me = { pos: new THREE.Vector3(12, 0, 12), vy: 0, onGround: false };
const keys = new Set();
addEventListener("keydown", e => {
  keys.add(e.code);
  const n = Number(e.key);
  if (n >= 1 && n <= state.kinds.length) { state.kind = state.kinds[n - 1]; renderKinds(); }
});
addEventListener("keyup", e => keys.delete(e.code));

const highlight = new THREE.LineSegments(edgesGeometry, new THREE.LineBasicMaterial({ color: "#ffffff" }));
highlight.scale.setScalar(1.01);
highlight.visible = false;
scene.add(highlight);
const raycaster = new THREE.Raycaster();
raycaster.far = REACH;

function aim() {
  raycaster.setFromCamera(new THREE.Vector2(0, 0), camera);
  const hit = raycaster.intersectObjects([...cubes.values(), ...floors], false)[0];
  if (!hit) return null;
  if (floors.includes(hit.object)) {
    const x = Math.floor(hit.point.x), y = Math.floor(hit.point.z);
    return { place: { x, y }, break: null, box: new THREE.Vector3(x + 0.5, 0.5, y + 0.5) };
  }
  const c = hit.object.userData;
  const n = hit.face.normal;
  return { place: { x: c.x + Math.round(n.x), y: c.y + Math.round(n.z) }, break: { x: c.x, y: c.y }, box: hit.object.position };
}

renderer.domElement.addEventListener("mousedown", e => {
  if (!controls.isLocked) return;
  const target = aim();
  if (!target) return;
  if (e.button === 0) send({ op: "place", x: target.place.x, y: target.place.y, kind: state.kind });
  if (e.button === 2 && target.break) send({ op: "break", x: target.break.x, y: target.break.y });
});
renderer.domElement.addEventListener("contextmenu", e => e.preventDefault());

function collides(p) {
  for (let x = Math.floor(p.x - RADIUS); x <= Math.floor(p.x + RADIUS); x++)
    for (let z = Math.floor(p.z - RADIUS); z <= Math.floor(p.z + RADIUS); z++)
      for (let y = Math.floor(p.y); y <= Math.floor(p.y + HEIGHT - 0.01); y++)
        if (solid(x, y, z)) return true;
  return false;
}

function move(dt) {
  const forward = new THREE.Vector3();
  camera.getWorldDirection(forward);
  forward.y = 0;
  forward.normalize();
  const right = new THREE.Vector3().crossVectors(forward, camera.up);
  const wish = new THREE.Vector3();
  if (keys.has("KeyW")) wish.add(forward);
  if (keys.has("KeyS")) wish.sub(forward);
  if (keys.has("KeyD")) wish.add(right);
  if (keys.has("KeyA")) wish.sub(right);
  if (wish.lengthSq() > 0) wish.normalize().multiplyScalar(SPEED * dt);
  if (keys.has("Space") && me.onGround) me.vy = JUMP;
  me.vy -= GRAVITY * dt;

  for (const axis of ["x", "z"]) {
    const next = me.pos.clone();
    next[axis] += wish[axis];
    if (!collides(next)) me.pos.copy(next);
  }
  const next = me.pos.clone();
  next.y += me.vy * dt;
  me.onGround = false;
  if (collides(next)) {
    if (me.vy < 0) { me.onGround = true; me.pos.y = Math.ceil(next.y); }
    me.vy = 0;
  } else me.pos.copy(next);
}

let lastSent = 0, lastPose = "";
const clock = new THREE.Clock();

function frame() {
  const dt = Math.min(clock.getDelta(), 0.05);
  const now = performance.now();
  if (controls.isLocked && state.placed) { unstick(); move(dt); }
  const here = roomOfRegion(regionAt(me.pos.x));
  if (state.placed && here && here !== state.room && !state.switching)
    enter(here, false).catch(e => { state.switching = false; log(e.message); });
  camera.position.set(me.pos.x, me.pos.y + EYE, me.pos.z);

  const target = controls.isLocked ? aim() : null;
  highlight.visible = !!target;
  if (target) highlight.position.copy(target.box);

  const yaw = new THREE.Euler().setFromQuaternion(camera.quaternion, "YXZ").y;
  const pose = `${me.pos.x.toFixed(2)},${me.pos.z.toFixed(2)},${me.pos.y.toFixed(2)},${yaw.toFixed(2)}`;
  if (now - lastSent > 100 && pose !== lastPose) {
    send({ op: "move", x: me.pos.x, y: me.pos.z, z: me.pos.y, yaw });
    lastSent = now;
    lastPose = pose;
  }

  for (const avatar of avatars.values()) {
    avatar.position.lerp(avatar.userData.target, Math.min(1, dt * 8));
    avatar.rotation.y = avatar.userData.yaw;
  }
  for (const mesh of cubes.values())
    for (const child of mesh.children) {
      const age = now - child.userData.born;
      child.material.opacity = Math.max(0, 1 - age / 3000);
      if (age > 3000) mesh.remove(child);
    }

  const w = $("view").clientWidth, h = $("view").clientHeight;
  if (renderer.domElement.width !== Math.floor(w * devicePixelRatio)) {
    renderer.setSize(w, h);
    camera.aspect = w / h;
    camera.updateProjectionMatrix();
  }
  renderer.render(scene, camera);
  requestAnimationFrame(frame);
}

paintFloors();
camera.rotation.set(-0.3, Math.PI * 0.75, 0, "YXZ");
renderInventory();
requestAnimationFrame(frame);
$("name").value = sessionStorage.getItem("cubeworld.name") || "";
$("join").onsubmit = async e => {
  e.preventDefault();
  try {
    await signIn($("name").value.trim());
    const rooms = await refreshServers();
    if (rooms.length) await enter(rooms[0].room_name);
    setInterval(() => refreshServers().catch(() => {}), 5000);
  } catch (err) {
    log(err.message);
  }
};
window.cubeworld = { state, enter, send, cubes, avatars, me, camera };
