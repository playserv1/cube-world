// One builder: a player like any other, signed in through the platform as a guest, in the room of the region it
// stands in, walking with the clients' own physics (web/physics.js) and sending the frames a browser client sends
// (the protocol part follows tools/guards/bot.mjs). On top: it walks a path of cells, aims with the client's ray,
// places and breaks blocks, puts up a pillar of scaffolding under itself and takes it down again.

import * as S from "../web/spec.js";
import { createBody, tick as physicsTick, knockback, eyeHeight } from "../web/physics.js";
import { refusal, roomOf } from "../web/rooms.js";
import { withKinds } from "../web/kinds.js";
import { key, aimPlace, aimDig, rayHit, overlaps } from "./world.mjs";
import { standable } from "./path.mjs";
import { now, sleep, after, every } from "./clock.mjs";

export const PLACE_GAP_MS = 200;     // a held right button repeats every 4 ticks

export class Bot {
  constructor({ cfg, name, world, inside, log, net }) {
    Object.assign(this, { cfg, name, world, inside, log });
    this.net = net ?? { fetch: (...a) => fetch(...a), WebSocket };
    this.slugs = cfg.slugs ?? [cfg.slug, `${cfg.slug}-ue`];
    this.player = null; this.socket = null; this.room = null; this.switching = false;
    this.regions = []; this.roomSlugs = {}; this.notBefore = {}; this.crossAfter = 0;
    this.inventory = {}; this.health = S.MAX_HEALTH; this.dead = false; this.placed = false;
    this.body = createBody(0, 0, 0);
    this.input = { forward: 0, strafe: 0, jump: false, sneak: false, sprint: false };
    this.yaw = 0; this.pitch = 0; this.lastPose = ""; this.moveSeq = 0;
    this.path = null; this.arrived = null; this.progress = 0; this.from = null; this.jumpNow = false;
    this.waiting = [];                 // keys of blocks placed and not yet heard back, oldest first
    this.stats = { placed: 0, dug: 0, relogs: 0, scaffold: 0 };
  }

  get id() { return this.player?.player_id; }
  get eye() { return { x: this.body.x, y: this.body.z, z: this.body.y + eyeHeight(this.body) }; }

  // ── platform ─────────────────────────────────────────────────────────────────────────────────

  async api(method, path, body, retried = false) {
    const headers = { "X-PlayServ-Client": this.cfg.clientKey, "Content-Type": "application/json" };
    if (this.player) headers.Authorization = `Bearer ${this.player.access_token}`;
    const res = await this.net.fetch(`${this.cfg.api}${path}`, { method, headers, body: body && JSON.stringify(body) });
    const json = await res.json().catch(() => ({}));
    if (res.status === 401 && !retried && this.player?.refresh_token) { await this.refresh(); return this.api(method, path, body, true); }
    if (!res.ok) throw Object.assign(new Error(`${method} ${path} → ${res.status} ${json.code || json.title || ""}`), { status: res.status, code: json.code });
    return json;
  }

  async refresh() {
    const json = await this.api("POST", "/auth/players/refresh", { refresh_token: this.player.refresh_token }, true);
    Object.assign(this.player, { access_token: json.access_token, refresh_token: json.refresh_token });
    this.scheduleRefresh((new Date(json.expires_at) - Date.now()) / 1000);
  }

  scheduleRefresh(seconds) {
    clearTimeout(this.refreshTimer);
    this.refreshTimer = setTimeout(() => this.refresh().catch(e => this.log(`${this.name}: ${e.message}`)), Math.max(10, (seconds || 900) - 60) * 1000);
  }

  async signIn() {
    this.player = await this.api("POST", "/auth/players/anon", { display_name: this.name });
    this.scheduleRefresh(this.player.expires_in);
  }

  /** The rooms both room types list now. */
  async browse() {
    const pages = await Promise.all(this.slugs.map(slug => this.api("GET", `/rooms/${slug}:browse`)
      .then(page => page.data.map(room => ({ ...room, slug })), e => { if (e.status === 404) return []; throw e; })));
    const rooms = pages.flat();
    for (const room of rooms) this.roomSlugs[room.room_name] = room.slug;
    return rooms;
  }

  /** Joins a room; resolves once that server's welcome comes (web/app.js enter). */
  async enter(roomName, teleport) {
    if (roomName === this.room || this.switching) return;
    this.switching = true;
    try {
      let slug = this.roomSlugs[roomName] ?? this.cfg.slug, ticket;
      try { ticket = await this.api("POST", `/rooms/${slug}/${roomName}:join`, {}); }
      catch (e) {
        if (e.status !== 404) throw e;
        slug = this.slugs.find(s => s !== slug) ?? slug;
        ticket = await this.api("POST", `/rooms/${slug}/${roomName}:join`, {});
      }
      this.roomSlugs[roomName] = slug;
      const c = ticket.connect, door = ticket.attributes?.ws;
      const url = door ? door : c ? `${c.transport === "wss" ? "wss" : "ws"}://${c.host}:${c.port}/` : `${this.cfg.api.replace(/^http/, "ws")}/games/${slug}`;
      const socket = new this.net.WebSocket(url);
      this.joining = socket;
      const giveUp = setTimeout(() => { if (this.socket !== socket) socket.close(); }, 10000);
      await new Promise((resolve, reject) => {
        socket.onopen = () => socket.send(JSON.stringify({ playerId: this.player.player_id, displayName: this.name, token: this.player.access_token, reservationToken: ticket.reservation_token }));
        socket.onmessage = e => {
          const frame = withKinds(JSON.parse(e.data));
          if (frame.type === "welcome" && this.socket !== socket) {
            clearTimeout(giveUp);
            const previous = this.socket;
            this.socket = socket; this.room = roomName; this.switching = false; this.joining = null;
            previous?.close();
            this.onFrame(frame, teleport || !this.placed);
            this.lastPose = "";
            resolve();
            return;
          }
          if (this.socket === socket) this.onFrame(frame);
        };
        socket.onerror = () => {};
        socket.onclose = e => {
          clearTimeout(giveUp);
          if (this.socket === socket) {
            const turned = refusal({ reason: e.reason });
            if (turned.message) this.notBefore[roomName] = Date.now() + turned.waitMs;
            this.room = null; this.socket = null;
            this.log(`${this.name}: left ${roomName} (${e.code} ${e.reason || ""})`);
          }
          if (this.joining === socket) { this.switching = false; this.joining = null; }
          reject(new Error(`socket to ${roomName} closed before the welcome`));
        };
      });
    } catch (e) {
      if (!this.joining || this.joining.readyState === 3) { this.switching = false; this.joining = null; }
      throw e;
    }
  }

  /**
   * Out of a block: leaves and comes back as a new guest, who starts with 64 of each (the servers keep a player's
   * inventory between visits, so only a new player starts full). It keeps its place: a server takes a first move from
   * within its region as it comes. The next tick joins the room of the region it stands in.
   */
  async relogin() {
    this.switching = true;
    const old = this.socket;
    this.socket = null; this.room = null;
    old?.close();
    clearTimeout(this.refreshTimer);
    try { this.player = null; await this.signIn(); this.stats.relogs++; }
    finally { this.switching = false; this.crossAfter = 0; }
    for (let i = 0; i < 100 && !this.room; i++) await sleep(100);
  }

  send(message) { if (this.socket?.readyState === 1) this.socket.send(JSON.stringify(message)); }

  start() { this.timer = every(S.TICK_MS, () => this.tick()); }
  stop() { clearInterval(this.timer); clearTimeout(this.refreshTimer); this.socket?.close(); }

  // ── frames ───────────────────────────────────────────────────────────────────────────────────

  onFrame(frame, teleport) {
    switch (frame.type) {
      case "welcome":
        this.regions = frame.regions ?? [];
        this.inventory = frame.inventory;
        this.health = frame.you.health; this.dead = false; this.moveSeq = 0;
        if (!this.world.ready) this.world.configure(frame);
        if (teleport) this.spawn(frame.you);
        this.send({ op: "bombs" });               // blocks batched in one "cubes" frame, as web/app.js asks
        break;
      case "regions": this.regions = frame.regions; break;
      case "correct":
        if (this.switching) break;
        this.moveSeq = frame.seq;
        Object.assign(this.body, { x: frame.x, y: frame.z, z: frame.y, px: frame.x, py: frame.z, pz: frame.y, vx: 0, vy: 0, vz: 0 });
        this.lastPose = "";
        this.abandon();
        break;
      case "cube": this.world.apply(frame.op, frame.cube); this.heard(frame.cube); break;
      case "cubes": for (const { op, cube } of frame.changes) { this.world.apply(op, cube); this.heard(cube); } break;
      case "inventory": this.inventory = frame.inventory; break;
      case "refused":
        this.inventory = frame.inventory;
        if (frame.op === "place" && this.waiting.length) this.world.forget(this.waiting.shift());
        break;
      case "hurt":
        if (frame.player === this.id) { this.health = frame.health; if (frame.strength) knockback(this.body, frame.kx, frame.ky, frame.strength); }
        break;
      case "death":
        if (frame.player === this.id) { this.dead = true; this.health = 0; this.abandon(); after(1500, () => this.send({ op: "respawn" })); }
        break;
      case "respawn":
        this.dead = false; this.health = frame.you.health; this.spawn(frame.you); this.abandon();
        break;
    }
  }

  heard(cube) { const i = this.waiting.indexOf(key(cube.x, cube.y, cube.z)); if (i >= 0) this.waiting.splice(i, 1); }

  spawn(at) {
    Object.assign(this.body, { x: at.x, y: at.z, z: at.y, px: at.x, py: at.z, pz: at.y, vx: 0, vy: 0, vz: 0, peak: at.z, onGround: false });
    this.placed = true;
  }

  // ── getting about ────────────────────────────────────────────────────────────────────────────

  /** The cell the body stands in. A 0.6 wide body can stand over a block's edge: then the cell that holds it up. */
  cell() {
    const b = this.body, z = Math.round(b.y), own = { x: Math.floor(b.x), y: Math.floor(b.z), z };
    if (standable(this.world, this.inside, own.x, own.y, z)) return own;
    for (const [dx, dz] of [[-0.3, -0.3], [0.3, -0.3], [-0.3, 0.3], [0.3, 0.3]]) {
      const c = { x: Math.floor(b.x + dx), y: Math.floor(b.z + dz), z };
      if (standable(this.world, this.inside, c.x, c.y, z)) return c;
    }
    return own;
  }

  /** Walks a path of cells; true once there, false if it got stuck or the way closed. */
  go(path) {
    if (!path.length) return Promise.resolve(true);
    this.progress = 0;
    this.from = this.cell();
    return new Promise(resolve => { this.arrived = ok => { this.arrived = null; resolve(ok); }; this.path = [...path]; });
  }
  abandon() { this.path = null; this.arrived?.(false); }

  async settle() { for (let i = 0; i < 40 && !this.body.onGround; i++) await sleep(S.TICK_MS); }

  /** One 50 ms tick: steer along the path, run the client's physics, tell the server, cross rooms as the client does. */
  tick() {
    if (!this.placed || !this.world.ready) return;
    const b = this.body;
    this.input = { forward: 0, strafe: 0, jump: false, sneak: false, sprint: false };
    if (this.jumpNow && b.onGround) { this.input.jump = true; this.jumpNow = false; }
    if (this.path?.length && !this.dead) {
      const w = this.path[0], next = this.path[1], from = this.from;
      if (!standable(this.world, this.inside, w.x, w.y, w.z)) this.abandon();
      else {
        const dx = w.x + 0.5 - b.x, dz = w.y + 0.5 - b.z, d = Math.hypot(dx, dz);
        // A turn, a drop, a jump or the end: come in slowly, so the 0.6 wide body stays inside its cell.
        const turning = !next || next.z !== w.z || next.x - w.x !== w.x - from.x || next.y - w.y !== w.y - from.y;
        if (d < (turning ? 0.12 : 0.3) && b.onGround && Math.abs(b.y - w.z) < 0.05) {
          this.from = this.path.shift();
          this.progress = 0;
          if (!this.path.length) { this.path = null; this.arrived?.(true); }
        } else {
          this.yaw = Math.atan2(-dx, dz); this.pitch = 0;
          this.input.forward = turning && d < 0.7 ? Math.max(0.12, d * 0.8) : 1;
          // a straight run of two cells or more: sprint, as a player holding Shift does (5.6 m/s, the server allows 10)
          this.input.sprint = !turning && this.path.length >= 2 && this.input.forward === 1;
          if (w.z > b.y + 0.5 && b.onGround && d < 1.4) this.input.jump = true;
          if (++this.progress > 80) this.abandon();
        }
      }
    }
    if (!this.dead) {
      physicsTick(b, { ...this.input, yaw: this.yaw }, this.world.physicsSolid);
      const pose = `${b.x.toFixed(3)},${b.y.toFixed(3)},${b.z.toFixed(3)},${this.yaw.toFixed(3)},${this.pitch.toFixed(3)},${b.onGround}`;
      this.idle = pose === this.lastPose ? (this.idle ?? 0) + 1 : 0;
      if (pose !== this.lastPose || this.idle % 20 === 0) {
        this.send({ op: "move", x: b.x, y: b.z, z: b.y, yaw: this.yaw, pitch: this.pitch, onGround: b.onGround, sneaking: false, sprinting: b.sprinting,
          seq: this.moveSeq, ...(b.onGround ? {} : { peak: b.peak }) });
        this.lastPose = pose;
      }
    }
    const r = roomOf(this.regions, this.world.regionOf(b.x, b.z));
    if (r?.slug && !this.roomSlugs[r.room]) this.roomSlugs[r.room] = r.slug;
    const t = Date.now();
    if (r && r.room !== this.room && !this.switching && t >= this.crossAfter && t >= (this.notBefore[r.room] ?? 0))
      this.enter(r.room, false).catch(e => {
        this.switching = false;
        const turned = refusal({ code: e.code });
        if (turned.message) this.notBefore[r.room] = t + turned.waitMs; else this.crossAfter = t + 3000;
      });
  }

  // ── hands ────────────────────────────────────────────────────────────────────────────────────

  look(hit) { this.yaw = hit.yaw; this.pitch = hit.pitch; }

  /**
   * Places b from where the body stands, as a held right button does: aim, send, draw it at once, and go on after
   * 4 ticks without waiting for the server (a refusal takes the block back). The finer aiming grid only if asked.
   */
  async place(b, { fine = false, everyone = [] } = {}) {
    if ((this.inventory[b.kind] ?? 0) <= 0) return "no stock";
    if (everyone.some(o => o.body && overlaps(o.body, b))) return "someone in the way";
    const hit = aimPlace(this.world, this.eye, b) ?? (fine ? aimPlace(this.world, this.eye, b, true) : null);
    if (!hit) return "could not aim";
    this.look(hit);
    await sleep(S.TICK_MS);
    const k = key(b.x, b.y, b.z);
    this.send({ op: "place", x: hit.x, y: hit.y, z: hit.z, nx: hit.nx, ny: hit.ny, nz: hit.nz, kind: b.kind });
    this.world.expect(k, b.kind, now());
    this.waiting.push(k);
    this.inventory[b.kind] = (this.inventory[b.kind] ?? 1) - 1;
    this.stats.placed++;
    await sleep(PLACE_GAP_MS - S.TICK_MS);
    return "ok";
  }

  /** Breaks b: holds the button on it until the server says it is gone. */
  async dig(b, { fine = true } = {}) {
    const hit = aimDig(this.world, this.eye, b) ?? (fine ? aimDig(this.world, this.eye, b, true) : null);
    if (!hit) return "could not aim";
    this.look(hit);
    const kind = this.world.onServer(b.x, b.y, b.z);
    this.send({ op: "dig", state: "start", x: b.x, y: b.y, z: b.z });
    const until = now() + this.world.breakTicks(kind) * S.TICK_MS + 2500;
    while (this.world.onServer(b.x, b.y, b.z) !== "air" && now() < until) await sleep(40);
    const gone = this.world.onServer(b.x, b.y, b.z) === "air";
    if (!gone) this.send({ op: "dig", state: "stop", x: b.x, y: b.y, z: b.z });
    await sleep(S.DIG_COOLDOWN_TICKS * S.TICK_MS);
    if (gone) this.stats.dug++;
    return gone ? "ok" : "not broken";
  }

  /** Pillars up as a player does: looking straight down, jump, and at the top of the jump place dirt where the feet were. */
  async pillarUp(height, scaffold) {
    while (this.cell().z < height) {
      await this.settle();
      const c = this.cell();
      if (this.world.solid(c.x, c.y, c.z + 2) || (this.inventory.dirt ?? 0) <= 0) return false;
      if (!(await this.go([c]))) return false;
      await this.settle();
      this.pitch = Math.PI / 2;
      this.jumpNow = true;
      const until = now() + 1000;
      while (this.body.y < c.z + 1.02 && now() < until) await sleep(5);
      if (this.body.y < c.z + 1.02) return false;
      const hit = rayHit(this.world, this.eye, { x: c.x + 0.5, y: c.y + 0.5, z: c.z });
      if (!hit || hit.x !== c.x || hit.y !== c.y || hit.z !== c.z - 1 || hit.nz !== 1) return false;
      const k = key(c.x, c.y, c.z);
      this.send({ op: "place", x: hit.x, y: hit.y, z: hit.z, nx: hit.nx, ny: hit.ny, nz: hit.nz, kind: "dirt" });
      this.world.expect(k, "dirt", now());
      this.waiting.push(k);
      scaffold.add(k);
      this.stats.scaffold++;
      await sleep(100);
      await this.settle();
      await sleep(150);
      if (this.world.onServer(c.x, c.y, c.z) !== "dirt") { scaffold.delete(k); return false; }
    }
    this.pitch = 0;
    return true;
  }

  /** Digs its scaffolding out from under its feet, a block at a time, down to whatever it stands on. */
  async digDown(scaffold) {
    for (;;) {
      await this.settle();
      const c = this.cell(), below = { x: c.x, y: c.y, z: c.z - 1 };
      if (!scaffold.has(key(below.x, below.y, below.z))) break;
      if ((await this.dig(below)) !== "ok") return false;
      scaffold.delete(key(below.x, below.y, below.z));
    }
    this.pitch = 0;
    return true;
  }
}
