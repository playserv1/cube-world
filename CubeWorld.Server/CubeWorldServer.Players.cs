using PlayServ.Sdk;

namespace CubeWorld.Server;

/// <summary>What a player does: joins, moves, digs, places, fights, dies and comes back.</summary>
public sealed partial class CubeWorldServer
{
    // ── joining ─────────────────────────────────────────────────────────────────────────────────────

    /// <summary>
    /// The inventory is theirs from wherever they last played; a first-timer gets a full stack of everything. The row is
    /// only read: writing it straight back raced the old server's last write after a crossing and could undo it.
    /// </summary>
    private async Task<(Inventory Inventory, bool New)> LoadInventory(string playerId)
    {
        for (var attempt = 0; ; attempt++)
        {
            try
            {
                var saved = await Platform.Table<CubeInventory>().FindByAsync(i => i.player_id, playerId);
                return saved?.Fields is { } record ? (Inventory.Parse(record.stacks), false) : (Inventory.Starting(), true);
            }
            catch (Exception e) when (attempt < 2)
            {
                _ = Platform.Log($"inventory of {playerId} not read, again in 2 s: {e.Message}");
                await Task.Delay(TimeSpan.FromSeconds(2));
            }
        }
    }

    private WorldPresence Spawn(string id, string name) => new()
    {
        player_id = id, name = name, server = _server, color = Color,
        x = World.Centre(_region).X, y = World.Centre(_region).Y, z = 0, health = Spec.MaxHealth,
    };

    /// <summary>Everything the client needs to draw the world: the rules, the changed blocks, the bombs, the inventory.</summary>
    private void SendWelcome(Player player)
    {
        WorldCube[] world;
        object[] bombs;
        lock (_world)
        {
            world = _world.Overrides.ToArray();
            player.Bomb = _bombs.Values.FirstOrDefault(b => b.Record.state == Bomb.Held && b.Record.holder == player.Pose.player_id)?.Record.bomb_id;
            bombs = _bombs.Values.Select(b => BombFrame(b.Record, b)).ToArray();
        }
        Send(player.Session, new
        {
            type = "welcome", server = _server, color = Color, region = _region, regions = _regions, you = player.Pose,
            width = World.Width, depth = World.Depth, regionSize = World.RegionSize, minZ = World.MinZ, maxZ = World.MaxZ,
            layers = Spec.Layers.Select(l => new { l.z, l.kind }), trees = Spec.Trees.Select(t => new { t.x, t.y }),
            blocks = Spec.Blocks.Select(b => new { kind = b.Kind, b.Hardness, b.NeedsTool, b.Transparent, b.Gravity, b.Drop, breakTicks = b.Breakable ? b.BreakTicks : -1 }),
            hotbar = Spec.Placeable, world, inventory = player.Inventory.Stacks, tick = _tick, bombs,
        });
    }

    // ── moving ──────────────────────────────────────────────────────────────────────────────────────

    private void Move(Player player, Command command)
    {
        if (player.Dead) return;
        var pose = player.Pose;
        pose.x = Math.Clamp(command.x, 0, World.Width);
        pose.y = Math.Clamp(command.y, 0, World.Depth);
        pose.z = Math.Clamp(command.z, World.MinZ, World.MaxZ + 8);
        pose.yaw = command.yaw;
        pose.pitch = command.pitch;
        pose.sneaking = command.sneaking ? 1 : 0;
        pose.sprinting = command.sprinting ? 1 : 0;
        player.Moved = true;

        // Fall damage, from the height reached since the player last stood on the ground (on this server or, over a
        // border, the one before: the client says its own peak).
        var damage = player.Fall.Step(pose.z, command.onGround, command.peak);
        if (damage > 0) Hurt(player, damage, null, 0, null);
    }

    // ── digging ─────────────────────────────────────────────────────────────────────────────────────

    private void Dig(Player player, Command command)
    {
        StopDig(player);
        if (command.state != "start" || player.Dead) return;

        var (x, y, z) = command.Block;
        var block = _world.BlockAt(x, y, z);
        if (!World.Inside(x, y, z) || !block.Solid || !block.Breakable || !CanReach(player, x, y, z)) return;

        player.Dig = new DigState(x, y, z, _tick, block.BreakTicks);
        ShowCrack(player, player.Dig, stage: 0);
    }

    /// <summary>Every tick: the crack grows by the block's hardness, and when it is through, the block breaks.</summary>
    private void ProgressDigging(Player player)
    {
        lock (_world)
        {
            if (player.Dig is not { } dig) return;
            if (player.Dead || !CanReach(player, dig.X, dig.Y, dig.Z))
            {
                StopDig(player);
                return;
            }

            var elapsed = _tick - dig.StartTick;
            var stage = (int)Math.Min(9, elapsed * 10 / dig.Ticks);
            if (stage != dig.Stage)
            {
                dig.Stage = stage;
                ShowCrack(player, dig, stage);
            }
            if (elapsed < dig.Ticks) return;

            StopDig(player);
            if (_world.Break(dig.X, dig.Y, dig.Z, player.Pose.player_id, _server) is not { } broken) return;

            if (broken.Broken.Drop is { } drop && player.Inventory.Give(drop)) ShareInventory(player);
            Publish(broken.Update);
        }
    }

    private void StopDig(Player player)
    {
        if (player.Dig is not { } dig) return;
        player.Dig = null;
        ShowCrack(player, dig, stage: -1);
    }

    /// <summary>The crack every player sees on the block: 0 to 9, or -1 when it is gone.</summary>
    private void ShowCrack(Player player, DigState dig, int stage) =>
        Broadcast(new { type = "dig", player = player.Pose.player_id, x = dig.X, y = dig.Y, z = dig.Z, stage });

    // ── placing ─────────────────────────────────────────────────────────────────────────────────────

    private void Place(Player player, Command command)
    {
        if (player.Dead) return;
        var (x, y, z) = command.Block;
        var kind = command.kind ?? "";
        var update = CanReach(player, x, y, z) && player.Inventory.Count(kind) > 0
            ? _world.Place(x, y, z, command.nx, command.ny, command.nz, kind, player.Pose.player_id, _server, EveryoneAlive().Select(HitboxOf))
            : null;

        if (update is null)
        {
            Send(player.Session, new { type = "refused", op = "place", inventory = player.Inventory.Stacks });
            return;
        }
        player.Inventory.Take(kind);
        ShareInventory(player);
        Publish(update);
    }

    private void ShareInventory(Player player)
    {
        WriteInventory(player);
        Send(player.Session, new { type = "inventory", inventory = player.Inventory.Stacks });
    }

    private void WriteInventory(Player player)
    {
        player.Sync.Wrote(player.Inventory.Stacks, Now);
        Platform.RuntimeData.Write(Uplink, "CubeInventory", player.Pose.player_id, player.Inventory.ToRecord(player.Pose.player_id));
    }

    /// <summary>
    /// A player's row came over the uplink: this server's own write coming back, or another writer's (the old server's
    /// last write after a crossing, the refill function's top-up), whose change is merged into what the player holds
    /// here. A row for a player whose own is still being read waits for that read (it is the newer).
    /// </summary>
    private void HearInventory(CubeInventory row)
    {
        // Only a whole row is a row to merge: one without its stacks would read as the starting stacks.
        if (string.IsNullOrEmpty(row.stacks)) return;
        if (_loading.TryGetValue(row.player_id, out var waiting) && _loading.TryUpdate(row.player_id, row, waiting)) return;
        if (!_players.TryGetValue(row.player_id, out var player)) return;
        var theirs = Inventory.Parse(row.stacks).Stacks;
        lock (_world)
        {
            if (player.Sync.Heard(player.Inventory.Stacks, theirs, Now) is not { } merged) return;
            var changedHere = !InventorySync.Same(merged, player.Inventory.Stacks);
            player.Inventory.Set(merged);
            // The row lacks what the player did here (another writer's row came after this server's): it is written again.
            if (!InventorySync.Same(merged, theirs)) WriteInventory(player);
            if (changedHere) Send(player.Session, new { type = "inventory", inventory = player.Inventory.Stacks });
        }
    }

    // ── fighting ────────────────────────────────────────────────────────────────────────────────────

    /// <summary>A hit on whoever is within reach: a player on this server, or one another server hosts.</summary>
    private void Attack(Player player, Command command)
    {
        if (player.Dead || command.target is null || command.target == player.Pose.player_id) return;
        var local = _players.GetValueOrDefault(command.target);
        var pose = local?.Pose ?? _elsewhere.GetValueOrDefault(command.target);
        if (pose is null || local is { Dead: true } || pose.health <= 0 || (local is null && Now - pose.seen_at > 5000)) return;

        // A hand recharges in 5 ticks; a hit before that is weaker: 20 % plus 80 % of the charge squared.
        var charge = Math.Min(1.0, (_tick - player.LastAttackTick) / (double)Spec.FistChargeTicks);
        player.LastAttackTick = _tick;

        if (!CanReach(player, pose)) return;

        var damage = Spec.FistDamage * (0.2 + 0.8 * charge * charge);
        double dx = pose.x - player.Pose.x, dy = pose.y - player.Pose.y;
        var length = Math.Sqrt(dx * dx + dy * dy);
        if (length < 1e-4) { dx = -Math.Sin(player.Pose.yaw); dy = Math.Cos(player.Pose.yaw); length = 1; }
        var strength = Spec.Knockback + (player.Pose.sprinting == 1 ? Spec.SprintKnockback : 0);

        if (local is not null) { Hurt(local, damage, (dx / length, dy / length), strength, player.Pose.player_id); return; }

        HitElsewhere($"{player.Pose.player_id}:{_tick}:{Guid.NewGuid():N}", command.target, player.Pose.player_id,
            damage, (dx / length, dy / length), strength);
    }

    /// <summary>The victim is on another server: the hit goes over through platform data, and that server applies it.</summary>
    private static void HitElsewhere(string id, string victim, string? by, double damage, (double x, double y) direction, double strength)
    {
        var hit = new WorldHit
        {
            hit_id = id, victim = victim, attacker = by ?? "", damage = damage, kx = direction.x, ky = direction.y, strength = strength, at = Now,
        };
        Platform.RuntimeData.Write(Uplink, "WorldHit", hit.hit_id, hit);
    }

    /// <summary>Another server's player hit one of ours: this server applies it and deletes the hit.</summary>
    private void HearHit(WorldHit hit)
    {
        if (hit.at is not { } at || Now - at >= 5000 || !_players.TryGetValue(hit.victim, out var victim)) return;
        lock (_world) Hurt(victim, hit.damage, (hit.kx, hit.ky), hit.strength, hit.attacker);
        Platform.RuntimeData.Delete(Uplink, "WorldHit", hit.hit_id);
    }

    private void Hurt(Player victim, double damage, (double x, double y)? direction, double strength, string? by)
    {
        if (victim.Dead || _tick - victim.LastHurtTick < Spec.InvulnerabilityTicks) return;
        victim.LastHurtTick = _tick;
        victim.Pose.health = Math.Max(0, victim.Pose.health - damage);
        victim.Moved = true;
        Broadcast(new
        {
            type = "hurt", player = victim.Pose.player_id, health = victim.Pose.health, by,
            kx = direction?.x * strength ?? 0, ky = direction?.y * strength ?? 0, strength = direction is null ? 0 : strength,
        });

        if (victim.Pose.health > 0) return;
        victim.Dead = true;
        StopDig(victim);
        Broadcast(new { type = "death", player = victim.Pose.player_id, by });
    }

    private void RegenerateHealth(Player player)
    {
        if (player.Dead || player.Pose.health >= Spec.MaxHealth || _tick % Spec.RegenIntervalTicks != 0) return;
        player.Pose.health = Math.Min(Spec.MaxHealth, player.Pose.health + 1);
        player.Moved = true;
    }

    private void Respawn(Player player)
    {
        if (!player.Dead) return;
        var spawn = Spawn(player.Pose.player_id, player.Pose.name);
        player.Pose.x = spawn.x; player.Pose.y = spawn.y; player.Pose.z = spawn.z;
        player.Pose.health = Spec.MaxHealth;
        player.Dead = false;
        player.Fall = new PlayerFall();
        player.Moved = true;
        Send(player.Session, new { type = "respawn", you = player.Pose });
    }

    // ── bodies ──────────────────────────────────────────────────────────────────────────────────────

    /// <summary>The players the other servers host, as they last said, if they said it in the last 5 s.</summary>
    private IEnumerable<WorldPresence> Others() =>
        _elsewhere.Values.Where(p => Now - p.seen_at < 5000 && !_players.ContainsKey(p.player_id));

    private IEnumerable<WorldPresence> OthersAlive() => Others().Where(p => p.health > 0);

    private IEnumerable<WorldPresence> EveryoneAlive() =>
        _players.Values.Where(p => !p.Dead).Select(p => p.Pose).Concat(OthersAlive());

    private static Hitbox HitboxOf(WorldPresence pose) =>
        new(pose.x, pose.y, pose.z, pose.sneaking == 1 ? Spec.SneakHeight : Spec.PlayerHeight);

    private static bool CanReach(Player player, int x, int y, int z)
    {
        var (ex, ey, ez) = Eye(player);
        return World.DistanceToBlock(ex, ey, ez, x, y, z) <= Spec.BlockReach + Spec.ReachTolerance;
    }

    private static bool CanReach(Player player, WorldPresence other)
    {
        var (ex, ey, ez) = Eye(player);
        return World.DistanceToHitbox(ex, ey, ez, HitboxOf(other)) <= Spec.EntityReach + Spec.ReachTolerance;
    }

    private static (double x, double y, double z) Eye(Player player) => (player.Pose.x, player.Pose.y, player.Pose.z + EyeHeightOf(player.Pose));

    private static double EyeHeightOf(WorldPresence pose) => pose.sneaking == 1 ? Spec.SneakEyeHeight : Spec.EyeHeight;

    private sealed record DigState(int X, int Y, int Z, long StartTick, int Ticks)
    {
        public int Stage { get; set; }
    }

    private sealed record Player(PlayerSession Session, Inventory Inventory, WorldPresence Pose)
    {
        public InventorySync Sync { get; init; } = new(Inventory.Stacks);
        public bool Moved { get; set; }
        public bool Dead { get; set; }
        public PlayerFall Fall { get; set; } = new();
        public long LastAttackTick { get; set; } = long.MinValue / 2;
        public long LastHurtTick { get; set; } = long.MinValue / 2;
        public DigState? Dig { get; set; }
        /// <summary>The bomb in the player's hand. A player holds one at a time and can only throw it.</summary>
        public string? Bomb { get; set; }
    }
}
