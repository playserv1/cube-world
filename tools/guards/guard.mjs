// What a guard does, tick by tick. Two guards stand by the entrance, one on each side. The surface guard answers
// for the roof and the entrance's frame, the vault guard for the rooms and the corridor under them; each puts back
// what someone broke or placed in its part. Both watch whoever comes up to the entrance. The vault guard follows a
// visitor down, a few steps behind, and goes back to its post when they come out; whoever breaks something down
// there is beaten to death, and then the room is put back. The owner (room.json `owner`) is not watched, followed
// or fought: the guards bow their heads as he passes, and what he builds becomes the blueprint.

import { findPath, standingCell, standable } from "./path.mjs";

const ATTACK_EVERY = 10;          // ticks: a victim cannot be hurt again for 10 ticks (Spec.InvulnerabilityTicks)
const ENTITY_REACH = 2.8;         // blocks from the eye to the hitbox: 3 on the server, a little kept back
const BLOCK_REACH = 4.2;          // blocks from the eye to the block: 4.5 on the server
const WATCH_RADIUS = 8;           // blocks from the entrance: someone this near is watched
const BOW_RADIUS = 4;             // blocks from the guard: the owner this near gets a bow
const FOLLOW_MIN = 3, FOLLOW_MAX = 5;
const CHASE_LIMIT = 30;           // blocks from the entrance: a vandal who runs further is let go
const RESPAWN_AFTER = 60;         // ticks
const REPATH_EVERY = 10;          // ticks
const MISSING_FOR = 200;
const REBUILD_FROM = 20;          // missing blocks: this many is a rebuild, said in the log
const RELOGIN_EVERY = 200;
const MAX_TRIES = 5, PUT_ASIDE = 600;   // tries at one cell, then ticks it waits        // ticks between two sign-ins for full stacks          // ticks a vandal may be missing from the list before the chase ends

const dist2 = (a, b) => Math.hypot(a.x - b.x, a.y - b.y);

export class Guard {
  /**
   * @param bot    the guard's player (bot.mjs)
   * @param role   "surface" or "vault"
   * @param room   the Room both guards keep
   * @param shared what the two guards know together: { guards: Set of ids, vandals: Map id → since }
   */
  constructor({ bot, role, room, shared, log, onBlueprint }) {
    Object.assign(this, { bot, role, room, shared, log, onBlueprint });
    this.post = room.spec.posts[role];
    this.t = 0;
    this.nav = null;
    this.task = null;
    this.lastAttack = -ATTACK_EVERY;
    this.lastPlace = -10;
    this.deadSince = null;
    this.skip = new Map();       // cell key → tick: a cell the server refused, tried again later
    bot.on(frame => this.onFrame(frame));
    // A guard that signed in again for full stacks is a new player: the other guard must know it too.
    bot.onIdentity(id => this.shared.guards.add(id));
  }

  get world() { return this.bot.world; }

  solid = (x, y, z) => this.world.isSolid(x, y, z);

  isOwner(p) { return !!p && (p.name ?? "").trim().toLowerCase() === this.room.spec.owner.toLowerCase(); }

  isGuard(id) { return this.shared.guards.has(id); }

  /** The players a guard pays attention to: alive, not a guard, not the owner. */
  strangers() {
    return [...this.bot.players.values()].filter(p => !this.isGuard(p.player_id) && !this.isOwner(p) && (p.health ?? 20) > 0);
  }

  // ── what the guard hears ─────────────────────────────────────────────────────────────────────

  onFrame(frame) {
    if (frame.type === "dig" && frame.stage >= 0) this.heardDamage(frame.player, frame.x, frame.y, frame.z, "digs");
    if (frame.type === "cube") this.heardChange(frame.op, frame.cube);
    if (frame.type === "cubes") for (const { op, cube } of frame.changes) this.heardChange(op, cube);
    if (frame.type === "death" && this.shared.vandals.delete(frame.player)) this.log(`${this.bot.name}: ${this.nameOf(frame.player)} is dead`);
  }

  heardChange(op, cube) {
    const { x, y, z } = cube;
    const want = this.room.wanted(x, y, z);
    if (want === undefined) return;
    const by = cube.placed_by, kind = op === "delete" ? this.world.generated(x, y, z) : cube.kind;
    // Only one guard writes the blueprint, the vault guard; the surface guard hears the same frames.
    if (this.role === "vault" && this.isOwner(this.bot.players.get(by)) && kind !== want) {
      this.room.adopt(x, y, z, kind);
      this.onBlueprint?.();
      return;
    }
    if (kind !== want) this.heardDamage(by, x, y, z, kind === "air" ? "broke" : `put ${kind} in`);
  }

  /** Someone damages the room. Breaking anything down below, or anything at all from inside, earns a beating. */
  heardDamage(by, x, y, z, what) {
    if (this.role !== "vault" || !by || this.isGuard(by)) return;
    const p = this.bot.players.get(by);
    if (!p || this.isOwner(p) || !this.room.inBox(x, y, z)) return;
    if (this.room.partOf(z) !== "vault" && !this.room.isInside(p)) return;
    if (!this.shared.vandals.has(by)) this.log(`${this.bot.name}: ${p.name} ${what} the room at ${x},${y},${z}`);
    this.shared.vandals.set(by, this.t);
  }

  nameOf(id) { return this.bot.players.get(id)?.name ?? id; }

  // ── the tick ─────────────────────────────────────────────────────────────────────────────────

  tick() {
    this.t++;
    const bot = this.bot;
    bot.input = { forward: 0, strafe: 0, jump: false, sneak: false, sprint: false };
    bot.walkYaw = null;
    if (!bot.world.ready || !bot.placed) return;

    if (bot.dead) {
      this.stopDig();
      this.deadSince ??= this.t;
      if (this.t - this.deadSince === RESPAWN_AFTER) bot.respawn();
      return;
    }
    this.deadSince = null;

    const vandal = this.role === "vault" ? this.currentVandal() : null;
    if (vandal) { this.mode = `hunt ${vandal.name}`; this.stopDig(); this.hunt(vandal); return; }

    const visitor = this.role === "vault" ? this.visitorInside() : null;
    if (visitor) { this.mode = `follow ${visitor.name}`; this.stopDig(); this.follow(visitor); return; }

    if (this.repair()) { this.mode = "repair"; return; }
    this.mode = "post";
    this.guardPost();
  }

  // ── fighting ─────────────────────────────────────────────────────────────────────────────────

  currentVandal() {
    const entrance = this.room.spec.entrance;
    for (const [id, since] of this.shared.vandals) {
      const p = this.bot.players.get(id);
      // Missing for a moment is not gone: a server the guard has just crossed to lists the others a little later.
      if (!p) {
        if (this.t - since > MISSING_FOR) { this.log(`${this.bot.name}: lost ${id}`); this.shared.vandals.delete(id); }
        continue;
      }
      this.shared.vandals.set(id, this.t);
      if ((p.health ?? 20) <= 0) { this.shared.vandals.delete(id); continue; }
      if (dist2(p, entrance) > CHASE_LIMIT) { this.log(`${this.bot.name}: ${p.name} got away`); this.shared.vandals.delete(id); continue; }
      return p;
    }
    return null;
  }

  hunt(p) {
    const target = { x: p.x, y: p.y, z: p.z + 1.2 };
    this.bot.lookAt(target);
    if (this.reachToPlayer(p) <= ENTITY_REACH) {
      if (this.t - this.lastAttack >= ATTACK_EVERY) {
        this.lastAttack = this.t;
        this.attacks = (this.attacks ?? 0) + 1;
        this.bot.attack(p.player_id);
      }
      // Stay on them as the knockback pushes them away.
      this.walkTowards(p, true);
      return;
    }
    const goal = c => Math.hypot(c.x + 0.5 - p.x, c.y + 0.5 - p.y) < 1.6 && Math.abs(c.z - p.z) < 1.5;
    if (!this.go(`hunt:${Math.floor(p.x)}:${Math.floor(p.y)}:${Math.floor(p.z)}`, goal, true)) this.walkTowards(p, true);
  }

  reachToPlayer(p) {
    const e = this.bot.eye, half = 0.3, h = p.sneaking ? 1.5 : 1.8;
    const dx = Math.max(0, Math.max(p.x - half - e.x, e.x - (p.x + half)));
    const dy = Math.max(0, Math.max(p.y - half - e.y, e.y - (p.y + half)));
    const dz = Math.max(0, Math.max(p.z - e.z, e.z - (p.z + h)));
    return Math.hypot(dx, dy, dz);
  }

  // ── following ────────────────────────────────────────────────────────────────────────────────

  visitorInside() {
    const me = this.bot.pos;
    const inside = this.strangers().filter(p => this.room.isInside(p));
    inside.sort((a, b) => dist2(a, me) - dist2(b, me));
    return inside[0] ?? null;
  }

  follow(p) {
    this.bot.lookAt({ x: p.x, y: p.y, z: p.z + 1.62 });
    const d = dist2(this.bot.pos, p);
    if (d >= FOLLOW_MIN && d <= FOLLOW_MAX + 1 && Math.abs(this.bot.pos.z - p.z) < 1.5) { this.nav = null; return; }
    const goal = c => { const g = Math.hypot(c.x + 0.5 - p.x, c.y + 0.5 - p.y); return g >= FOLLOW_MIN && g <= FOLLOW_MAX && Math.abs(c.z - p.z) < 1.5; };
    this.go(`follow:${Math.floor(p.x)}:${Math.floor(p.y)}:${Math.floor(p.z)}`, goal, false, true);
  }

  // ── keeping the room ─────────────────────────────────────────────────────────────────────────

  /** Works on the nearest damaged cell of the guard's part. Returns false when there is nothing to do. */
  repair() {
    const all = this.room.damage(this.world, this.role);
    const damage = all.filter(c => (this.skip.get(`${c.x}:${c.y}:${c.z}`) ?? -1) <= this.t);
    if (this.task && !damage.some(c => c.x === this.task.x && c.y === this.task.y && c.z === this.task.z)) this.stopDig();
    // A room that is not on the map at all (a new world, a reset) is built from nothing, block by block, the same way.
    if (all.length >= REBUILD_FROM && !this.rebuilding) { this.rebuilding = true; this.log(`${this.bot.name}: ${all.length} blocks of the ${this.role} part are missing, building it`); }
    if (this.tries) for (const k of [...this.tries.keys()]) if (!all.some(c => `${c.x}:${c.y}:${c.z}` === k)) this.tries.delete(k);
    if (!all.length && this.rebuilding) { this.rebuilding = false; this.log(`${this.bot.name}: the ${this.role} part is whole`); }
    if (!damage.length) return false;

    // A block can be placed only against a solid one: rebuild from what still stands.
    const me = this.bot.pos;
    const doable = damage.filter(c => c.now !== "air" || this.support(c));
    if (!doable.length) return false;
    doable.sort((a, b) => Math.hypot(a.x + 0.5 - me.x, a.y + 0.5 - me.y, a.z + 0.5 - me.z) - Math.hypot(b.x + 0.5 - me.x, b.y + 0.5 - me.y, b.z + 0.5 - me.z));
    const cell = this.task && doable.find(c => c.x === this.task.x && c.y === this.task.y && c.z === this.task.z) || doable[0];
    this.target = { ...cell, left: damage.length };

    this.bot.lookAt({ x: cell.x + 0.5, y: cell.y + 0.5, z: cell.z + 0.5 });
    if (!this.canReachBlock(cell) || this.inMyBody(cell)) {
      this.stopDig();
      const goal = c => this.reachFrom(c, cell) <= BLOCK_REACH - 0.5 && !this.cellInBody(c, cell);
      if (!this.go(`repair:${cell.x}:${cell.y}:${cell.z}`, goal)) this.skip.set(`${cell.x}:${cell.y}:${cell.z}`, this.t + 100);
      return true;
    }
    this.nav = null;

    // Something is in the way of the blueprint: dig it out first.
    if (cell.now !== "air") {
      if (this.world.breakTicks(cell.now) < 0) { this.skip.set(`${cell.x}:${cell.y}:${cell.z}`, this.t + 400); return true; }
      if (!this.task || this.task.x !== cell.x || this.task.y !== cell.y || this.task.z !== cell.z || this.t - this.task.since > this.world.breakTicks(cell.now) + 40) {
        this.stopDig();
        if (this.tooManyTries(cell)) return true;
        this.task = { ...cell, since: this.t };
        this.bot.dig(cell.x, cell.y, cell.z, "start");
      }
      return true;
    }

    this.stopDig();
    if (!(this.bot.inventory[cell.want] > 0)) {
      // Out of it: come back as a new player, who starts with full stacks. Not more than once in 10 s.
      if (!this.relogging && this.t - (this.reloggedAt ?? -RELOGIN_EVERY) >= RELOGIN_EVERY) {
        this.relogging = true;
        this.reloggedAt = this.t;
        this.log(`${this.bot.name}: out of ${cell.want}, signing in again`);
        this.bot.relogin().catch(e => this.log(`${this.bot.name}: ${e.message}`)).finally(() => { this.relogging = false; });
      }
      return true;
    }
    if (this.t - this.lastPlace < 4) return true;
    const s = this.support(cell);
    this.lastPlace = this.t;
    if (this.tooManyTries(cell)) return true;
    this.bot.place(s.x, s.y, s.z, cell.x - s.x, cell.y - s.y, cell.z - s.z, cell.want);
    // If the server refuses (someone stands there), the cell waits a little before the next try.
    this.skip.set(`${cell.x}:${cell.y}:${cell.z}`, this.t + 8);
    return true;
  }

  /**
   * A cell the server keeps refusing (someone stands in it, or this guard's picture of the world is behind the
   * server's) is put aside for a while instead of being tried on every tick.
   */
  tooManyTries(cell) {
    const k = `${cell.x}:${cell.y}:${cell.z}`, tries = this.tries ??= new Map();
    const n = (tries.get(k) ?? 0) + 1;
    tries.set(k, n);
    if (n <= MAX_TRIES) return false;
    tries.delete(k);
    this.skip.set(k, this.t + PUT_ASIDE);
    this.log(`${this.bot.name}: ${k} does not take, trying it again later`);
    return true;
  }

  stopDig() {
    if (!this.task) return;
    this.bot.dig(this.task.x, this.task.y, this.task.z, "stop");
    this.task = null;
  }

  /** A solid neighbour to place against. */
  support({ x, y, z }) {
    for (const [dx, dy, dz] of [[0, 0, -1], [1, 0, 0], [-1, 0, 0], [0, 1, 0], [0, -1, 0], [0, 0, 1]])
      if (this.solid(x + dx, y + dy, z + dz) && this.world.inside(x + dx, y + dy, z + dz)) return { x: x + dx, y: y + dy, z: z + dz };
    return null;
  }

  canReachBlock(cell) { return this.distanceToBlock(this.bot.eye, cell) <= BLOCK_REACH; }

  reachFrom(stand, cell) { return this.distanceToBlock({ x: stand.x + 0.5, y: stand.y + 0.5, z: stand.z + 1.62 }, cell); }

  distanceToBlock(e, { x, y, z }) {
    const dx = Math.max(0, Math.max(x - e.x, e.x - (x + 1)));
    const dy = Math.max(0, Math.max(y - e.y, e.y - (y + 1)));
    const dz = Math.max(0, Math.max(z - e.z, e.z - (z + 1)));
    return Math.hypot(dx, dy, dz);
  }

  inMyBody(cell) {
    if (cell.now !== "air") return false;
    const p = this.bot.pos;
    return p.x + 0.3 > cell.x && p.x - 0.3 < cell.x + 1 && p.y + 0.3 > cell.y && p.y - 0.3 < cell.y + 1 && p.z + 1.8 > cell.z && p.z < cell.z + 1;
  }

  cellInBody(stand, cell) { return stand.x === cell.x && stand.y === cell.y && (stand.z === cell.z || stand.z + 1 === cell.z); }

  // ── the post ─────────────────────────────────────────────────────────────────────────────────

  guardPost() {
    const post = this.post, me = this.bot.pos;
    const atPost = dist2(me, post) < 0.6 && Math.abs(me.z - post.z) < 1;
    if (!atPost) {
      const goal = c => c.x === Math.floor(post.x) && c.y === Math.floor(post.y);
      if (this.go("post", goal)) return;
      this.walkTowards(post);
      return;
    }
    this.nav = null;
    this.watch();
  }

  /** At the post: bow to the owner as he passes, look at whoever comes up to the entrance, or look out. */
  watch() {
    const me = this.bot.pos, entrance = this.room.spec.entrance;
    const owner = [...this.bot.players.values()].find(p => this.isOwner(p) && (p.health ?? 20) > 0 && dist2(p, me) <= BOW_RADIUS);
    if (owner) {
      this.bot.lookAt({ x: owner.x, y: owner.y, z: owner.z + 1.62 });
      this.bot.pitch = 0.9;
      return;
    }
    const near = this.strangers().filter(p => dist2(p, entrance) <= WATCH_RADIUS).sort((a, b) => dist2(a, me) - dist2(b, me))[0];
    if (near) { this.bot.lookAt({ x: near.x, y: near.y, z: near.z + 1.62 }); return; }
    this.bot.yaw = post_yaw(this.post);
    this.bot.pitch = 0;
  }

  // ── walking ──────────────────────────────────────────────────────────────────────────────────

  /** Walks the shortest way to the nearest cell `goal` accepts. Returns false when there is no way. */
  go(key, goal, sprint = false, keepLook = false) {
    const from = standingCell(this.solid, this.bot.pos);
    if (goal(from) && standable(this.solid, from.x, from.y, from.z)) {
      // In the goal cell: settle into its middle.
      this.steer({ x: from.x + 0.5, y: from.y + 0.5, z: from.z }, false, keepLook, 0.25);
      return true;
    }
    const stuck = this.nav && this.t - this.nav.progressAt > 30;
    if (!this.nav || this.nav.key !== key || this.t - this.nav.at > REPATH_EVERY * 3 || stuck) {
      const area = this.room.spec.area;
      const path = findPath(this.solid, from, goal, c => c.x >= area.x0 && c.x <= area.x1 && c.y >= area.y0 && c.y <= area.y1);
      this.nav = { key, path, i: 1, at: this.t, progressAt: this.t };
      if (!path) return false;
    }
    const nav = this.nav;
    if (!nav.path) return false;
    while (nav.i < nav.path.length) {
      const c = nav.path[nav.i], p = this.bot.pos;
      if (Math.hypot(c.x + 0.5 - p.x, c.y + 0.5 - p.y) < 0.4 && Math.abs(c.z - p.z) < 0.6) { nav.i++; nav.progressAt = this.t; continue; }
      break;
    }
    if (nav.i >= nav.path.length) { this.nav = null; return true; }
    const next = nav.path[nav.i];
    this.steer({ x: next.x + 0.5, y: next.y + 0.5, z: next.z }, sprint, keepLook);
    return true;
  }

  steer(to, sprint, keepLook, slow = 0) {
    const bot = this.bot, p = bot.pos;
    const dx = to.x - p.x, dy = to.y - p.y, d = Math.hypot(dx, dy);
    if (d < 0.08) return;
    bot.walkYaw = Math.atan2(-dx, dy);
    if (!keepLook && !this.task) { bot.yaw = bot.walkYaw; bot.pitch = 0.3; }
    bot.input.forward = slow && d < slow ? 0.3 : 1;
    bot.input.sprint = sprint && d > 1.5;
    bot.input.jump = (to.z > p.z + 0.5 && d < 1.4) || (bot.body.horizontalCollision && bot.body.onGround);
  }

  walkTowards(p, sprint = false) {
    const me = this.bot.pos;
    if (Math.hypot(p.x - me.x, p.y - me.y) < 0.5) return;
    this.steer({ x: p.x, y: p.y, z: p.z }, sprint, true);
  }
}

/** Where a guard at its post looks: the way its post says, or out of the entrance. */
function post_yaw(post) { return post.yaw ?? 0; }
