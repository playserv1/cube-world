// A stand-in for the platform and one C# game server, in this process, for a dry run: the builders talk to it
// through the same fetch and WebSocket calls they use against the real thing, and it keeps the rules the real
// server keeps (CubeWorldServer.Players.cs, World.cs, Spec.cs): reach 4.5 + 1 from the eyes to the block a
// placement goes against or the block being dug, placing only into air against a solid face and never into a
// player, only in its own region, stacks of 64 and the starting stacks for a new player, break times by hardness.
// Nothing here reaches the network, and nothing the dry run does touches the real world.

import { key, buildTreeMap } from "./world.mjs";
import { now, after, every } from "./clock.mjs";

// Spec.Blocks: kind, hardness, needs a tool, drop.
const BLOCKS = [
  ["air", 0, false, null], ["bedrock", -1, true, null], ["grass", 0.6, false, "dirt"], ["dirt", 0.5, false, "dirt"],
  ["sand", 0.5, false, "sand"], ["stone", 1.5, true, null], ["wood", 2, false, "wood"], ["brick", 2, true, null],
  ["glass", 0.3, false, null], ["gold", 3, true, null], ["leaves", 0.2, false, null],
].map(([kind, hardness, tool, drop]) => ({ kind, Hardness: hardness, NeedsTool: tool, Transparent: ["air", "glass", "leaves"].includes(kind), Gravity: kind === "sand", Drop: drop,
  breakTicks: hardness < 0 ? -1 : Math.ceil(hardness * (tool ? 100 : 30) - 1e-9) }));
const BY_KIND = new Map(BLOCKS.map(b => [b.kind, b]));
const PLACEABLE = BLOCKS.filter(b => b.kind !== "air" && b.kind !== "bedrock").map(b => b.kind);
const LAYERS = [{ z: -4, kind: "bedrock" }, { z: -3, kind: "dirt" }, { z: -2, kind: "dirt" }, { z: -1, kind: "grass" }];
const REACH = 4.5 + 1, EYE = 1.62, HEIGHT = 1.8, HALF = 0.3, CAPACITY = 16, LATENCY_MS = 40;
const COLORS = ["red", "blue", "green", "yellow", "purple", "pink"];

export class FakeServer {
  constructor({ color, seed = [] }) {
    this.region = COLORS.indexOf(color);
    this.x0 = (this.region % 3) * 24; this.y0 = Math.floor(this.region / 3) * 24;
    this.room = `${color}-dry`;
    this.width = 72; this.depth = 48;
    this.trees = [0, 1, 2, 3, 4, 5].flatMap(r => { const x0 = (r % 3) * 24, y0 = Math.floor(r / 3) * 24; return [[x0 + 4, y0 + 5], [x0 + 18, y0 + 4], [x0 + 6, y0 + 18], [x0 + 19, y0 + 17]].map(([x, y]) => ({ x, y })); });
    this.treeMap = buildTreeMap(this.trees);
    this.overrides = new Map(seed.map(c => [key(c.x, c.y, c.z), c.kind]));
    this.players = new Map();       // id -> { socket, pose, inventory, dig }
    this.tickCount = 0;
    this.counts = { placed: 0, refused: 0, broken: 0 };
    this.timer = every(50, () => this.tick());
    let n = 0;
    this.newId = () => `plr_dry${++n}`;
  }

  stop() { clearInterval(this.timer); }

  kindAt(x, y, z) {
    if (x < 0 || x >= this.width || y < 0 || y >= this.depth || z < -4 || z >= 64) return "air";
    const k = key(x, y, z);
    if (this.overrides.has(k)) return this.overrides.get(k);
    return z >= 0 ? this.treeMap.get(k) ?? "air" : LAYERS.find(l => l.z === z)?.kind ?? "air";
  }
  solid(x, y, z) { return this.kindAt(x, y, z) !== "air"; }
  inRegion(x, y) { return x >= this.x0 && x < this.x0 + 24 && y >= this.y0 && y < this.y0 + 24; }

  // ── the platform's HTTP side ───────────────────────────────────────────────────────────────

  fetch = async (url, { method = "GET", body } = {}) => {
    const path = new URL(url).pathname;
    const reply = (status, json) => ({ ok: status < 400, status, json: async () => json });
    if (method === "POST" && path === "/auth/players/anon") {
      const id = this.newId();
      return reply(200, { player_id: id, access_token: `tok_${id}`, refresh_token: `ref_${id}`, expires_in: 900 });
    }
    if (method === "POST" && path === "/auth/players/refresh") return reply(200, { access_token: "tok", refresh_token: "ref", expires_at: new Date(Date.now() + 900000).toISOString() });
    let m = path.match(/^\/rooms\/([^/:]+):browse$/);
    if (m) return m[1] === "cubeworld" ? reply(200, { data: [{ room_name: this.room, players: this.players.size, capacity: CAPACITY }] }) : reply(404, { code: "not_found" });
    m = path.match(/^\/rooms\/([^/]+)\/([^/:]+):join$/);
    if (m) {
      if (m[2] !== this.room) return reply(404, { code: "room_not_found" });
      if (this.players.size >= CAPACITY) return reply(409, { code: "room_full" });
      return reply(200, { reservation_token: `res_${Math.random()}`, connect: { host: "dry.run", port: 1, transport: "wss" } });
    }
    return reply(404, { code: "not_found" });
  };

  // ── the game socket ─────────────────────────────────────────────────────────────────────────

  get WebSocket() {
    const server = this;
    return class FakeSocket {
      constructor() { this.readyState = 0; this.player = null; after(LATENCY_MS, () => { this.readyState = 1; this.onopen?.(); }); }
      send(text) { const m = JSON.parse(text); after(LATENCY_MS, () => server.receive(this, m)); }
      close() { if (this.readyState === 3) return; this.readyState = 3; server.leave(this); after(1, () => this.onclose?.({ code: 1000, reason: "" })); }
      deliver(frame) { if (this.readyState === 1) after(LATENCY_MS, () => this.readyState === 1 && this.onmessage?.({ data: JSON.stringify(frame) })); }
    };
  }

  leave(socket) { if (socket.player) this.players.delete(socket.player); }
  broadcast(frame) { for (const p of this.players.values()) p.socket.deliver(frame); }

  receive(socket, m) {
    if (!socket.player) {
      if (this.players.size >= CAPACITY) { socket.close(); return; }
      const id = m.playerId;
      const p = { socket, name: m.displayName, inventory: Object.fromEntries(PLACEABLE.map(k => [k, 64])), dig: null,
        pose: { player_id: id, name: m.displayName, x: this.x0 + 12, y: this.y0 + 12, z: 0, yaw: 0, pitch: 0, health: 20, sneaking: 0, sprinting: 0 } };
      socket.player = id;
      this.players.set(id, p);
      socket.deliver({
        type: "welcome", server: "dry", color: COLORS[this.region], region: this.region,
        regions: [{ region: String(this.region), server: "dry", color: COLORS[this.region], room: this.room, slug: "cubeworld" }],
        you: p.pose, width: this.width, depth: this.depth, regionSize: 24, minZ: -4, maxZ: 64, layers: LAYERS, trees: this.trees,
        blocks: BLOCKS, hotbar: PLACEABLE, world: [...this.overrides].map(([k, kind]) => { const [x, y, z] = k.split(":").map(Number); return { key: k, x, y, z, kind }; }),
        inventory: p.inventory, tick: this.tickCount, bombs: [],
      });
      return;
    }
    const p = this.players.get(socket.player);
    if (!p) return;
    switch (m.op) {
      case "move": Object.assign(p.pose, { x: m.x, y: m.y, z: m.z, yaw: m.yaw, pitch: m.pitch }); break;
      case "place": this.place(p, m); break;
      case "dig": this.dig(p, m); break;
      case "respawn": break;
    }
  }

  reach(p, x, y, z) {
    const ex = p.pose.x, ey = p.pose.y, ez = p.pose.z + EYE;
    const dx = Math.max(0, x - ex, ex - (x + 1)), dy = Math.max(0, y - ey, ey - (y + 1)), dz = Math.max(0, z - ez, ez - (z + 1));
    return Math.hypot(dx, dy, dz) <= REACH;
  }

  place(p, m) {
    const ax = Math.floor(m.x), ay = Math.floor(m.y), az = Math.floor(m.z), x = ax + m.nx, y = ay + m.ny, z = az + m.nz;
    const kind = m.kind ?? "";
    const intersects = [...this.players.values()].some(o => { const q = o.pose; return q.x - HALF < x + 1 && q.x + HALF > x && q.y - HALF < y + 1 && q.y + HALF > y && q.z < z + 1 && q.z + HEIGHT > z; });
    const ok = this.reach(p, ax, ay, az) && this.inRegion(p.pose.x, p.pose.y) && (p.inventory[kind] ?? 0) > 0 && PLACEABLE.includes(kind)
      && this.solid(ax, ay, az) && Math.abs(m.nx) + Math.abs(m.ny) + Math.abs(m.nz) === 1 && !this.solid(x, y, z) && z >= -4 && z < 64 && !intersects;
    if (!ok) { this.counts.refused++; p.socket.deliver({ type: "refused", op: "place", inventory: p.inventory }); return; }
    p.inventory[kind]--;
    this.overrides.set(key(x, y, z), kind);
    this.counts.placed++;
    p.socket.deliver({ type: "inventory", inventory: p.inventory });
    this.broadcast({ type: "cubes", remote: false, falls: [], changes: [{ op: "upsert", cube: { key: key(x, y, z), x, y, z, kind } }] });
  }

  dig(p, m) {
    p.dig = null;
    if (m.state !== "start") return;
    const x = Math.floor(m.x), y = Math.floor(m.y), z = Math.floor(m.z), b = BY_KIND.get(this.kindAt(x, y, z));
    if (!b || b.kind === "air" || b.breakTicks < 0 || !this.reach(p, x, y, z) || !this.inRegion(p.pose.x, p.pose.y)) return;
    p.dig = { x, y, z, start: this.tickCount, ticks: b.breakTicks };
  }

  tick() {
    this.tickCount++;
    for (const p of this.players.values()) {
      const d = p.dig;
      if (!d) continue;
      if (!this.reach(p, d.x, d.y, d.z)) { p.dig = null; continue; }
      if (this.tickCount - d.start < d.ticks) continue;
      p.dig = null;
      const kind = this.kindAt(d.x, d.y, d.z), drop = BY_KIND.get(kind)?.Drop;
      this.overrides.set(key(d.x, d.y, d.z), "air");
      this.counts.broken++;
      if (drop && (p.inventory[drop] ?? 0) < 64) { p.inventory[drop] = (p.inventory[drop] ?? 0) + 1; p.socket.deliver({ type: "inventory", inventory: p.inventory }); }
      this.broadcast({ type: "cubes", remote: false, falls: [], changes: [{ op: "upsert", cube: { key: key(d.x, d.y, d.z), x: d.x, y: d.y, z: d.z, kind: "air" } }] });
    }
  }
}
