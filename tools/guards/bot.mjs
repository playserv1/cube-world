// One guard in the world: a player like any other, signed in through the platform, in the room of the region it
// stands in, walking with the clients' own physics and sending the frames a browser client sends. It crosses into
// the next server's room the way web/app.js does.

import * as S from "../../web/spec.js";
import { createBody, tick as physicsTick, knockback, eyeHeight } from "../../web/physics.js";
import { refusal, roomOf } from "../../web/rooms.js";
import { withKinds } from "../../web/kinds.js";

export class Bot {
  /** @param world a World shared by both guards: every frame from either server lands in it. */
  constructor({ cfg, name, world, log }) {
    Object.assign(this, { cfg, name, world, log });
    this.slugs = cfg.slugs ?? [cfg.slug, `${cfg.slug}-ue`];
    this.player = null;
    this.socket = null;
    this.room = null;
    this.switching = false;
    this.regions = [];
    this.roomSlugs = {};
    this.notBefore = {};
    this.crossAfter = 0;
    this.players = new Map();     // everyone in the world, by id, as the last "players" frame named them
    this.inventory = {};
    this.health = S.MAX_HEALTH;
    this.dead = false;
    this.placed = false;
    // The body in the clients' coordinates (x, z on the ground, y up), as web/physics.js keeps it.
    this.body = createBody(0, 0, 0);
    this.input = { forward: 0, strafe: 0, jump: false, sneak: false, sprint: false };
    this.yaw = 0;
    this.pitch = 0;
    this.lastPose = "";
    // The last correction this server sent (its move check), said back in every move: each server numbers its own.
    this.moveSeq = 0;
    this.handlers = [];
  }

  get id() { return this.player?.player_id; }

  /** Where the guard stands, in the server's coordinates: feet x, y and z. */
  get pos() { return { x: this.body.x, y: this.body.z, z: this.body.y }; }

  get eye() { return { x: this.body.x, y: this.body.z, z: this.body.y + eyeHeight(this.body) }; }

  on(handler) { this.handlers.push(handler); }

  // ── platform ─────────────────────────────────────────────────────────────────────────────────

  async api(method, path, body, retried = false) {
    const headers = { "X-PlayServ-Client": this.cfg.clientKey, "Content-Type": "application/json" };
    if (this.player) headers.Authorization = `Bearer ${this.player.access_token}`;
    const res = await fetch(`${this.cfg.api}${path}`, { method, headers, body: body && JSON.stringify(body) });
    const json = await res.json().catch(() => ({}));
    if (res.status === 401 && !retried && this.player?.refresh_token) { await this.refresh(); return this.api(method, path, body, true); }
    if (!res.ok) throw Object.assign(new Error(`${method} ${path} → ${res.status} ${json.code || json.title || ""}`), { status: res.status, code: json.code });
    return json;
  }

  async refresh() {
    const res = await fetch(`${this.cfg.api}/auth/players/refresh`, {
      method: "POST", headers: { "X-PlayServ-Client": this.cfg.clientKey, "Content-Type": "application/json" },
      body: JSON.stringify({ refresh_token: this.player.refresh_token }),
    });
    const json = await res.json().catch(() => ({}));
    if (!res.ok) throw new Error(`session refresh → ${res.status} ${json.code || json.title || ""}`);
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

  /**
   * Leaves and comes back as a new player, with the starting stacks: both servers keep a player's inventory from one
   * visit to the next, so only a new player starts full. The guard keeps its place: a server takes a first move from
   * within its region as it comes (MoveCheck.Arrive). The next tick joins the room of the region it stands in.
   */
  async relogin() {
    this.switching = true;
    const old = this.socket;
    this.socket = null;
    this.room = null;
    old?.close();
    clearTimeout(this.refreshTimer);
    try {
      this.player = null;
      await this.signIn();
      this.log(`${this.name}: signed in again as ${this.id}, for full stacks`);
      for (const h of this.identityHandlers ?? []) h(this.id);
    } finally {
      this.switching = false;
      this.crossAfter = 0;
    }
  }

  onIdentity(handler) { (this.identityHandlers ??= []).push(handler); }

  /** The rooms both room types list now, for the first entry. */
  async browse() {
    const pages = await Promise.all(this.slugs.map(slug => this.api("GET", `/rooms/${slug}:browse`)
      .then(page => page.data.map(room => ({ ...room, slug })), e => { if (e.status === 404) return []; throw e; })));
    const rooms = pages.flat();
    for (const room of rooms) this.roomSlugs[room.room_name] = room.slug;
    return rooms;
  }

  /** Joins a room; the socket becomes the guard's once that server's welcome comes. Resolves on the welcome. */
  async enter(roomName, teleport) {
    if (roomName === this.room || this.switching) return;
    this.switching = true;
    try {
      let slug = this.roomSlugs[roomName] ?? this.cfg.slug;
      let ticket;
      try { ticket = await this.api("POST", `/rooms/${slug}/${roomName}:join`, {}); }
      catch (e) {
        if (e.status !== 404) throw e;
        slug = this.slugs.find(s => s !== slug) ?? slug;
        ticket = await this.api("POST", `/rooms/${slug}/${roomName}:join`, {});
      }
      this.roomSlugs[roomName] = slug;
      const c = ticket.connect;
      const door = ticket.attributes?.ws;
      const url = door ? door : c ? `${c.transport === "wss" ? "wss" : "ws"}://${c.host}:${c.port}/` : `${this.cfg.api.replace(/^http/, "ws")}/games/${slug}`;
      const socket = new WebSocket(url);
      // Only the socket being opened ends the crossing when it closes. The room left behind closes its socket too, and
      // if that ended a crossing begun since, a second one would join twice: the server refuses one reservation as
      // invalid, the guard joins afresh, and a fresh join starts at the region's middle.
      this.joining = socket;
      // A server that has not welcomed the guard in 10 s is given up, as web/app.js does: it plays on where it is.
      const giveUp = setTimeout(() => { if (this.socket !== socket) socket.close(); }, 10000);
      await new Promise((resolve, reject) => {
        socket.onopen = () => socket.send(JSON.stringify({
          playerId: this.player.player_id, displayName: this.name,
          token: this.player.access_token, reservationToken: ticket.reservation_token,
        }));
        socket.onmessage = e => {
          const frame = withKinds(JSON.parse(e.data));
          if (frame.type === "welcome" && this.socket !== socket) {
            clearTimeout(giveUp);
            const previous = this.socket;
            this.socket = socket;
            this.room = roomName;
            this.switching = false;
            this.joining = null;
            previous?.close();
            this.onFrame(frame, teleport || !this.placed);
            this.lastPose = "";
            this.log(`${this.name}: on ${roomName}`);
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
            this.room = null;
            this.socket = null;
            this.log(`${this.name}: left ${roomName} (${e.code} ${e.reason || ""})`);
          }
          if (this.joining === socket) { this.switching = false; this.joining = null; }
          reject(new Error(`socket to ${roomName} closed before the welcome`));
        };
      });
    } catch (e) {
      if (!this.joining || this.joining.readyState === WebSocket.CLOSED) { this.switching = false; this.joining = null; }
      throw e;
    }
  }

  send(message) {
    if (this.socket?.readyState === WebSocket.OPEN) this.socket.send(JSON.stringify(message));
  }

  // ── frames ───────────────────────────────────────────────────────────────────────────────────

  onFrame(frame, teleport) {
    switch (frame.type) {
      case "welcome":
        this.regions = frame.regions ?? [];
        this.inventory = frame.inventory;
        this.health = frame.you.health;
        this.dead = false;
        this.moveSeq = 0;
        this.world.configure(frame);
        if (frame.players) this.players = new Map(frame.players.map(p => [p.player_id, p]));
        if (teleport) this.spawn(frame.you);
        // Blocks batched in one "cubes" frame, as web/app.js asks for them.
        this.send({ op: "bombs" });
        break;
      case "regions": this.regions = frame.regions; break;
      case "correct":
        // A move the server's check refused: back to where the last good move left the guard. Not while crossing:
        // the room being left is behind (web/app.js).
        if (this.switching) break;
        this.moveSeq = frame.seq;
        Object.assign(this.body, { x: frame.x, y: frame.z, z: frame.y, px: frame.x, py: frame.z, pz: frame.y, vx: 0, vy: 0, vz: 0 });
        this.lastPose = "";
        this.log(`${this.name}: corrected to ${frame.x.toFixed(1)},${frame.y.toFixed(1)},${frame.z.toFixed(1)}`);
        break;
      case "cube": this.world.apply(frame.op, frame.cube); break;
      case "cubes": for (const { op, cube } of frame.changes) this.world.apply(op, cube); break;
      case "inventory":
      case "refused": this.inventory = frame.inventory; break;
      case "players":
        this.players = new Map(frame.players.map(p => [p.player_id, p]));
        break;
      case "hurt":
        if (frame.player === this.id) {
          this.health = frame.health;
          if (frame.strength) knockback(this.body, frame.kx, frame.ky, frame.strength);
        }
        break;
      case "death":
        if (frame.player === this.id) { this.dead = true; this.health = 0; }
        break;
      case "respawn":
        this.dead = false;
        this.health = frame.you.health;
        this.spawn(frame.you);
        break;
    }
    for (const h of this.handlers) {
      try { h(frame, this); } catch (e) { this.log(`${this.name}: ${e.stack}`); }
    }
  }

  spawn(at) {
    Object.assign(this.body, { x: at.x, y: at.z, z: at.y, px: at.x, py: at.z, pz: at.y, vx: 0, vy: 0, vz: 0, peak: at.z, onGround: false });
    this.placed = true;
    let guard = 0;
    while (this.overlapsBlocks() && guard++ < 80) { this.body.y = Math.floor(this.body.y) + 1; this.body.py = this.body.y; }
  }

  overlapsBlocks() {
    const b = this.body, half = S.WIDTH / 2;
    for (let x = Math.floor(b.x - half); x <= Math.floor(b.x + half - 1e-7); x++)
      for (let z = Math.floor(b.z - half); z <= Math.floor(b.z + half - 1e-7); z++)
        for (let y = Math.floor(b.y + 1e-7); y <= Math.floor(b.y + S.HEIGHT - 1e-7); y++)
          if (this.world.isSolid(x, z, y)) return true;
    return false;
  }

  // ── what the guard does ──────────────────────────────────────────────────────────────────────

  /** Turns the head to look at a point (server coordinates). Minecraft's yaw: forward is (-sin yaw, cos yaw). */
  lookAt(p) {
    const e = this.eye, dx = p.x - e.x, dy = p.y - e.y, dz = p.z - e.z;
    if (Math.hypot(dx, dy) > 1e-3) this.yaw = Math.atan2(-dx, dy);
    this.pitch = -Math.atan2(dz, Math.hypot(dx, dy));
  }

  attack(target) { this.send({ op: "attack", target }); }

  place(ax, ay, az, nx, ny, nz, kind) { this.send({ op: "place", x: ax, y: ay, z: az, nx, ny, nz, kind }); }

  dig(x, y, z, state) { this.send({ op: "dig", state, x, y, z }); }

  respawn() { this.send({ op: "respawn" }); }

  /** One 50 ms tick: move with the current input, tell the server, cross into the next room when needed. */
  tick() {
    if (!this.placed || !this.world.ready) return;
    if (!this.dead) {
      const solid = (x, y, z) => this.world.isSolid(x, z, y);
      physicsTick(this.body, { ...this.input, yaw: this.walkYaw ?? this.yaw }, solid);
      const b = this.body;
      const pose = `${b.x.toFixed(3)},${b.y.toFixed(3)},${b.z.toFixed(3)},${this.yaw.toFixed(3)},${this.pitch.toFixed(3)},${b.onGround},${b.sneaking}`;
      if (pose !== this.lastPose) {
        // In the air the move says the fall's highest point too, as web/app.js does.
        this.send({ op: "move", x: b.x, y: b.z, z: b.y, yaw: this.yaw, pitch: this.pitch, onGround: b.onGround, sneaking: b.sneaking, sprinting: b.sprinting,
          seq: this.moveSeq, ...(b.onGround ? {} : { peak: b.peak }) });
        this.lastPose = pose;
      }
    }
    const r = roomOf(this.regions, this.world.regionOf(this.body.x, this.body.z));
    if (r?.slug && !this.roomSlugs[r.room]) this.roomSlugs[r.room] = r.slug;
    const now = Date.now();
    if (r && r.room !== this.room && !this.switching && now >= this.crossAfter && now >= (this.notBefore[r.room] ?? 0))
      this.enter(r.room, false).catch(e => {
        this.switching = false;
        const turned = refusal({ code: e.code });
        if (turned.message) this.notBefore[r.room] = now + turned.waitMs; else this.crossAfter = now + 3000;
      });
  }
}
