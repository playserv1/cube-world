using System.Collections.Concurrent;
using System.Text.Json;
using PlayServ.Sdk;
using PlayServ.Sdk.Data;
using PlayServ.Sdk.Rooms;

namespace CubeWorld.Server;

/// <summary>
/// The game server. It is the authority Minecraft's server is: it checks reach and the face a block is placed
/// against, times every dig by the block's hardness, deals damage and knockback, and ticks 20 times a second.
/// Movement itself is simulated by the client and reported back, as Minecraft clients do.
/// </summary>
public sealed class CubeWorldServer : PlatformGameServer
{
    private const string Uplink = "";

    private readonly World _world = new();
    private readonly ConcurrentDictionary<string, Player> _players = new();
    private readonly ConcurrentDictionary<string, WorldPresence> _elsewhere = new();
    private readonly Dictionary<string, LiveBomb> _bombs = new();
    private readonly Random _random = new();
    private readonly RoomHost<WorldRoom, WorldPlayer, object> _rooms = new(name => new WorldRoom(name), tickHz: 1);
    private readonly string _server = ServerName(Environment.GetEnvironmentVariable("PLAYSERV_MACHINE_ID"));
    private int _region = -1;
    private WorldRegion[] _regions = [];
    private long _tick;

    private string Color => _region >= 0 ? World.RegionColors[_region] : "grey";

    private string RoomName => $"{Color}-{_server}";

    private static long Now => DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();

    protected override TimeSpan ReconnectGrace => TimeSpan.Zero;

    protected override Task OnStartupAsync()
    {
        Platform.OnRuntimeDataUpdate(OnDataChanged);
        _ = Task.Run(RunAsync);
        return Task.CompletedTask;
    }

    private async Task RunAsync()
    {
        while (true)
        {
            try
            {
                // Subscribe before loading: a change written while the world loads arrives as an update
                // instead of being missed (applying one that the load already holds changes nothing).
                Subscribe();
                _world.Load(await LoadCubesAsync());
                foreach (var bomb in await LoadBombsAsync()) lock (_world) OnBomb(bomb, owned: false);
                _region = await ClaimRegionAsync();
                if (_region >= 0) break;
            }
            catch { }
            await Task.Delay(TimeSpan.FromSeconds(5));
        }
        await Platform.Log($"{RoomName}: {_world.Overrides.Count()} changed blocks loaded, {Spec.Trees.Length} oaks, {_bombs.Count} bombs, world ready");
        _ = Task.Run(ShareMovesAsync);
        _ = Task.Run(TickAsync);

        while (true)
        {
            if (_rooms.Find(RoomName) is null or { IsDisposed: true })
            {
                _rooms.Remove(RoomName);
                _rooms.GetOrCreate(RoomName);
                foreach (var player in _players.Values)
                    InRoom(room => room.AddPlayer(new WorldPlayer { Id = player.Pose.player_id, DisplayName = player.Pose.name }));
            }
            Platform.RuntimeData.Write(Uplink, "WorldRegion", $"{_region}", Claim(_region));
            try { _regions = await LiveRegionsAsync(); } catch { }
            Broadcast(new { type = "regions", regions = _regions });
            Subscribe();
            await Task.Delay(TimeSpan.FromSeconds(5));
        }
    }

    private static void Subscribe()
    {
        Platform.RuntimeData.Subscribe(Uplink, "WorldCube", "field:key");
        Platform.RuntimeData.Subscribe(Uplink, "CubeInventory", "field:player_id");
        Platform.RuntimeData.Subscribe(Uplink, "WorldPresence", "field:player_id");
        Platform.RuntimeData.Subscribe(Uplink, "WorldHit", "field:hit_id");
        Platform.RuntimeData.Subscribe(Uplink, "WorldBomb", "field:bomb_id");
    }

    private async Task<int> ClaimRegionAsync()
    {
        var regions = Platform.Table<WorldRegion>();
        for (var region = 0; region < World.RegionColors.Length; region++)
        {
            var holder = (await regions.FindByAsync(r => r.region, $"{region}"))?.Fields;
            if (holder is not null && holder.server != _server && Now - holder.seen_at < 30_000) continue;

            await regions.UpsertByAsync(r => r.region, $"{region}", UpsertMode.Managed, Claim(region));
            await Task.Delay(TimeSpan.FromSeconds(1));
            if ((await regions.FindByAsync(r => r.region, $"{region}"))?.Fields?.server == _server) return region;
        }
        return -1;
    }

    private WorldRegion Claim(int region) => new()
    {
        region = $"{region}", server = _server, color = World.RegionColors[region], room = $"{World.RegionColors[region]}-{_server}", seen_at = Now,
    };

    private static async Task<WorldRegion[]> LiveRegionsAsync()
    {
        var rows = await Platform.Table<WorldRegion>().Query().ToListAsync();
        return rows.Select(r => r.Fields!).Where(r => Now - r.seen_at < 30_000).ToArray();
    }

    private async Task ShareMovesAsync()
    {
        for (var tick = 0; ; tick++)
        {
            await Task.Delay(TimeSpan.FromMilliseconds(100));

            if (tick % 2 == 0)
                foreach (var player in _players.Values.Where(p => p.Moved || Now - p.Pose.seen_at > 2000))
                {
                    player.Moved = false;
                    player.Pose.seen_at = Now;
                    Platform.RuntimeData.Write(Uplink, "WorldPresence", player.Pose.player_id, player.Pose);
                }

            var everyone = _players.Values.Select(p => p.Pose)
                .Concat(_elsewhere.Values.Where(p => Now - p.seen_at < 5000 && !_players.ContainsKey(p.player_id)));
            Broadcast(new { type = "players", players = everyone });
        }
    }

    /// <summary>The 20 Hz game tick: digging progresses and finishes, health regenerates, bombs come down and fly.</summary>
    private async Task TickAsync()
    {
        var next = Environment.TickCount64;
        while (true)
        {
            next += 1000 / Spec.TicksPerSecond;
            var wait = next - Environment.TickCount64;
            if (wait > 0) await Task.Delay((int)wait); else next = Environment.TickCount64;
            _tick++;

            foreach (var player in _players.Values)
            {
                try
                {
                    lock (_world) TickDig(player);
                    if (!player.Dead && player.Pose.health < Spec.MaxHealth && _tick % Spec.RegenIntervalTicks == 0)
                    {
                        player.Pose.health = Math.Min(Spec.MaxHealth, player.Pose.health + 1);
                        player.Moved = true;
                    }
                }
                catch (Exception e) { _ = Platform.Log($"tick: {e.Message}"); }
            }
            try { lock (_world) TickBombs(); }
            catch (Exception e) { _ = Platform.Log($"bombs: {e.Message}"); }
        }
    }

    // ── bombs ────────────────────────────────────────────────────────────────────────────────────────

    /// <summary>
    /// Every server brings every free bomb down, but only the server of the region a bomb is over lets a player pick
    /// it up, so two servers never hand out one bomb. A thrown bomb is flown by its thrower's server alone.
    /// </summary>
    private void TickBombs()
    {
        foreach (var live in _bombs.Values.ToArray())
        {
            var bomb = live.Record;
            if (bomb.state == Bomb.Free)
            {
                live.Z = Bomb.Descend(_world, bomb.x, bomb.y, live.Z);
                if ((int)Math.Floor(bomb.x / World.RegionSize) != _region) continue;
                var taker = _players.Values.FirstOrDefault(p => p.Throws && !p.Dead && p.Bomb is null
                                                                && Bomb.InPickupReach(HitboxOf(p.Pose), bomb.x, bomb.y, live.Z));
                if (taker is not null)
                    Share(Next(bomb, Bomb.Held, taker.Pose.player_id, bomb.x, bomb.y, live.Z), owned: false);
            }
            else if (bomb.state == Bomb.Flying && live.Owned)
            {
                var flight = live.Age++ >= Spec.BombFlightTicks ? Flight.Exploded
                    : Bomb.Fly(_world, live.P, live.V, Targets(), bomb.holder, live.Age);
                if (flight == Flight.Exploded) Explode(bomb, live.P);
                else if (flight == Flight.Gone) Share(Next(bomb, Bomb.Fizzled, bomb.holder, live.P[0], live.P[1], live.P[2]), owned: false);
            }
        }
    }

    private void Throw(Player player, Command command)
    {
        if (player.Dead || player.Bomb is not { } id) return;
        player.Bomb = null;
        if (!_bombs.TryGetValue(id, out var live) || live.Record.state != Bomb.Held || live.Record.holder != player.Pose.player_id) return;

        double dx = command.x, dy = command.y, dz = command.z;
        var length = Math.Sqrt(dx * dx + dy * dy + dz * dz);
        if (length < 1e-6)
        {
            var (yaw, pitch) = (player.Pose.yaw, player.Pose.pitch);
            (dx, dy, dz, length) = (-Math.Sin(yaw) * Math.Cos(pitch), Math.Cos(yaw) * Math.Cos(pitch), -Math.Sin(pitch), 1);
        }
        var (ex, ey, ez) = Eye(player);
        var thrown = Next(live.Record, Bomb.Flying, player.Pose.player_id, ex, ey, ez);
        (thrown.vx, thrown.vy, thrown.vz) = (dx / length * Spec.ThrowSpeed, dy / length * Spec.ThrowSpeed, dz / length * Spec.ThrowSpeed);
        Share(thrown, owned: true);
    }

    /// <summary>
    /// Players here are hurt, players elsewhere get a WorldHit, both judged against the world as it stood before the
    /// blast, as Minecraft does; then the blocks go through platform data as every change does.
    /// </summary>
    private void Explode(WorldBomb bomb, double[] at)
    {
        var (cx, cy, cz) = (at[0], at[1], at[2]);

        foreach (var player in _players.Values.Where(p => !p.Dead))
            if (_world.Blast(cx, cy, cz, Spec.BombPower, HitboxOf(player.Pose), EyeHeightOf(player.Pose)) is { } blast)
                Hurt(player, blast.Damage, (blast.Nx, blast.Ny), blast.Impact, bomb.holder);

        foreach (var pose in _elsewhere.Values.Where(p => Now - p.seen_at < 5000 && p.health > 0 && !_players.ContainsKey(p.player_id)))
            if (_world.Blast(cx, cy, cz, Spec.BombPower, HitboxOf(pose), EyeHeightOf(pose)) is { } blast)
            {
                var hit = new WorldHit
                {
                    hit_id = $"{bomb.bomb_id}:{pose.player_id}", victim = pose.player_id, attacker = bomb.holder,
                    damage = blast.Damage, kx = blast.Nx, ky = blast.Ny, strength = blast.Impact, at = Now,
                };
                Platform.RuntimeData.Write(Uplink, "WorldHit", hit.hit_id, hit);
            }

        Publish(_world.Explode(cx, cy, cz, Spec.BombPower, _random, bomb.holder, _server));
        Share(Next(bomb, Bomb.Exploded, bomb.holder, cx, cy, cz), owned: false);
    }

    private static WorldBomb Next(WorldBomb bomb, string state, string holder, double x, double y, double z) => new()
    {
        bomb_id = bomb.bomb_id, state = state, holder = holder, x = x, y = y, z = z, dropped_at = bomb.dropped_at, at = Now,
    };

    private void Share(WorldBomb bomb, bool owned)
    {
        Platform.RuntimeData.Write(Uplink, "WorldBomb", bomb.bomb_id, bomb);
        OnBomb(bomb, owned);
    }

    /// <summary>A bomb moved on, here or on another server. Anything that does not move it forward is an echo or stale.</summary>
    private void OnBomb(WorldBomb bomb, bool owned)
    {
        var known = _bombs.GetValueOrDefault(bomb.bomb_id);
        if (known is not null && Bomb.Rank(bomb.state) <= Bomb.Rank(known.Record.state)) return;
        if (known is null && Bomb.Over(bomb.state) && Now - bomb.at > 5000) return;

        if (Bomb.Over(bomb.state)) _bombs.Remove(bomb.bomb_id);
        else _bombs[bomb.bomb_id] = LiveBomb.Of(bomb, _world, owned, Now);

        foreach (var player in _players.Values)
            if (player.Bomb == bomb.bomb_id && (bomb.state != Bomb.Held || bomb.holder != player.Pose.player_id)) player.Bomb = null;
        if (bomb.state == Bomb.Held && _players.TryGetValue(bomb.holder, out var holder)) holder.Bomb = bomb.bomb_id;

        Broadcast(BombFrame(bomb, _bombs.GetValueOrDefault(bomb.bomb_id)));
    }

    private static object BombFrame(WorldBomb bomb, LiveBomb? live) => new { type = "bomb", bomb, age = Now - bomb.at, z = live?.Z ?? bomb.z };

    private IEnumerable<(string, Hitbox)> Targets() =>
        _players.Values.Where(p => !p.Dead).Select(p => p.Pose)
            .Concat(_elsewhere.Values.Where(p => Now - p.seen_at < 5000 && p.health > 0 && !_players.ContainsKey(p.player_id)))
            .Select(p => (p.player_id, HitboxOf(p)));

    private void TickDig(Player player)
    {
        if (player.Dig is not { } dig) return;
        var (ex, ey, ez) = Eye(player);
        if (World.DistanceToBlock(ex, ey, ez, dig.X, dig.Y, dig.Z) > Spec.BlockReach + Spec.ReachTolerance || player.Dead)
        {
            StopDig(player);
            return;
        }

        var elapsed = _tick - dig.StartTick;
        var stage = (int)Math.Min(9, elapsed * 10 / dig.Ticks);
        if (stage != dig.Stage)
        {
            dig.Stage = stage;
            Broadcast(new { type = "dig", player = player.Pose.player_id, x = dig.X, y = dig.Y, z = dig.Z, stage });
        }
        if (elapsed < dig.Ticks) return;

        player.Dig = null;
        Broadcast(new { type = "dig", player = player.Pose.player_id, x = dig.X, y = dig.Y, z = dig.Z, stage = -1 });
        if (_world.Break(dig.X, dig.Y, dig.Z, player.Pose.player_id, _server) is not { } broken) return;

        if (broken.Broken.Drop is { } drop && player.Inventory.Give(drop)) ShareInventory(player);
        Publish(broken.Update);
    }

    private void StopDig(Player player)
    {
        if (player.Dig is not { } dig) return;
        player.Dig = null;
        Broadcast(new { type = "dig", player = player.Pose.player_id, x = dig.X, y = dig.Y, z = dig.Z, stage = -1 });
    }

    protected override async Task OnPlayerConnected(PlayerSession session)
    {
        var name = session.DisplayName ?? session.Id;
        var saved = await Platform.Table<CubeInventory>().FindByAsync(i => i.player_id, session.Id);
        var inventory = saved?.Fields is { } record ? Inventory.Parse(record.stacks) : Inventory.Starting();
        Platform.RuntimeData.Write(Uplink, "CubeInventory", session.Id, inventory.ToRecord(session.Id));

        var pose = Spawn(session.Id, name);
        var player = new Player(session, inventory, pose);
        _players[session.Id] = player;
        InRoom(room => room.AddPlayer(new WorldPlayer { Id = session.Id, DisplayName = name }));

        WorldCube[] world;
        object[] bombs;
        lock (_world)
        {
            world = _world.Overrides.ToArray();
            player.Bomb = _bombs.Values.FirstOrDefault(b => b.Record.state == Bomb.Held && b.Record.holder == session.Id)?.Record.bomb_id;
            bombs = _bombs.Values.Select(b => BombFrame(b.Record, b)).ToArray();
        }
        Send(session, new
        {
            type = "welcome", server = _server, color = Color, region = _region, regions = _regions, you = pose,
            width = World.Width, depth = World.Depth, regionSize = World.RegionSize, minZ = World.MinZ, maxZ = World.MaxZ,
            layers = Spec.Layers.Select(l => new { l.z, l.kind }), trees = Spec.Trees.Select(t => new { t.x, t.y }),
            blocks = Spec.Blocks.Select(b => new { kind = b.Kind, b.Hardness, b.NeedsTool, b.Transparent, b.Gravity, b.Drop, breakTicks = b.Breakable ? b.BreakTicks : -1 }),
            hotbar = Spec.Placeable, world, inventory = inventory.Stacks, tick = _tick, bombs,
        });
    }

    private WorldPresence Spawn(string id, string name) => new()
    {
        player_id = id, name = name, server = _server, color = Color,
        x = (_region + 0.5) * World.RegionSize, y = World.Depth / 2.0, z = 0, health = Spec.MaxHealth,
    };

    protected override Task OnPlayerMessage(PlayerSession session, byte[] message)
    {
        if (!_players.TryGetValue(session.Id, out var player) || Parse(message) is not { } command) return Task.CompletedTask;
        InRoom(room => room.MarkActive(room.GetPlayer(session.Id)));

        lock (_world)
        {
            switch (command.op)
            {
                case "move": Move(player, command); break;
                case "dig": Dig(player, command); break;
                case "place": Place(player, command); break;
                case "attack": Attack(player, command); break;
                case "respawn": Respawn(player); break;
                case "throw": Throw(player, command); break;
                case "bombs": player.Throws = true; break;
            }
        }
        return Task.CompletedTask;
    }

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

        // Fall damage, from the height reached since the player last stood on the ground.
        if (command.onGround)
        {
            if (player.Airborne)
            {
                var damage = Math.Ceiling(player.Peak - pose.z - Spec.SafeFallDistance);
                if (damage > 0) Hurt(player, damage, null, 0, null);
            }
            player.Airborne = false;
        }
        else
        {
            player.Peak = player.Airborne ? Math.Max(player.Peak, pose.z) : pose.z;
            player.Airborne = true;
        }
    }

    private void Dig(Player player, Command command)
    {
        StopDig(player);
        if (command.state != "start" || player.Dead) return;

        int x = (int)Math.Floor(command.x), y = (int)Math.Floor(command.y), z = (int)Math.Floor(command.z);
        var block = _world.BlockAt(x, y, z);
        var (ex, ey, ez) = Eye(player);
        if (!World.Inside(x, y, z) || !block.Solid || !block.Breakable
            || World.DistanceToBlock(ex, ey, ez, x, y, z) > Spec.BlockReach + Spec.ReachTolerance) return;

        player.Dig = new DigState(x, y, z, _tick, block.BreakTicks);
        Broadcast(new { type = "dig", player = player.Pose.player_id, x, y, z, stage = 0 });
    }

    private void Place(Player player, Command command)
    {
        if (player.Dead) return;
        int x = (int)Math.Floor(command.x), y = (int)Math.Floor(command.y), z = (int)Math.Floor(command.z);
        var kind = command.kind ?? "";
        var (ex, ey, ez) = Eye(player);
        var reachable = World.DistanceToBlock(ex, ey, ez, x, y, z) <= Spec.BlockReach + Spec.ReachTolerance;
        var update = reachable && player.Inventory.Count(kind) > 0
            ? _world.Place(x, y, z, command.nx, command.ny, command.nz, kind, player.Pose.player_id, _server, Hitboxes())
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

        var (ex, ey, ez) = Eye(player);
        if (World.DistanceToHitbox(ex, ey, ez, HitboxOf(pose)) > Spec.EntityReach + Spec.ReachTolerance) return;

        var damage = Spec.FistDamage * (0.2 + 0.8 * charge * charge);
        double dx = pose.x - player.Pose.x, dy = pose.y - player.Pose.y;
        var length = Math.Sqrt(dx * dx + dy * dy);
        if (length < 1e-4) { dx = -Math.Sin(player.Pose.yaw); dy = Math.Cos(player.Pose.yaw); length = 1; }
        var strength = Spec.Knockback + (player.Pose.sprinting == 1 ? Spec.SprintKnockback : 0);

        if (local is not null) { Hurt(local, damage, (dx / length, dy / length), strength, player.Pose.player_id); return; }

        // The victim is on another server: hand the hit over through platform data; that server applies it.
        var hit = new WorldHit
        {
            hit_id = $"{player.Pose.player_id}:{_tick}:{Guid.NewGuid():N}", victim = command.target, attacker = player.Pose.player_id,
            damage = damage, kx = dx / length, ky = dy / length, strength = strength, at = Now,
        };
        Platform.RuntimeData.Write(Uplink, "WorldHit", hit.hit_id, hit);
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

    private void Respawn(Player player)
    {
        if (!player.Dead) return;
        var spawn = Spawn(player.Pose.player_id, player.Pose.name);
        player.Pose.x = spawn.x; player.Pose.y = spawn.y; player.Pose.z = spawn.z;
        player.Pose.health = Spec.MaxHealth;
        player.Dead = false;
        player.Airborne = false;
        player.Moved = true;
        Send(player.Session, new { type = "respawn", you = player.Pose });
    }

    private void Publish(WorldUpdate update)
    {
        foreach (var fall in update.Falls)
            Broadcast(new { type = "fall", kind = fall.Kind, x = fall.X, y = fall.Y, fromZ = fall.FromZ, toZ = fall.ToZ });
        foreach (var (op, cube) in update.Changes)
        {
            if (op == "delete") Platform.RuntimeData.Delete(Uplink, "WorldCube", cube.key);
            else Platform.RuntimeData.Write(Uplink, "WorldCube", cube.key, cube);
            Broadcast(new { type = "cube", op, cube, remote = false });
        }
    }

    private void ShareInventory(Player player)
    {
        Platform.RuntimeData.Write(Uplink, "CubeInventory", player.Pose.player_id, player.Inventory.ToRecord(player.Pose.player_id));
        Send(player.Session, new { type = "inventory", inventory = player.Inventory.Stacks });
    }

    private IEnumerable<Hitbox> Hitboxes() =>
        _players.Values.Where(p => !p.Dead).Select(p => p.Pose)
            .Concat(_elsewhere.Values.Where(p => Now - p.seen_at < 5000 && !_players.ContainsKey(p.player_id)))
            .Select(HitboxOf);

    private static Hitbox HitboxOf(WorldPresence pose) =>
        new(pose.x, pose.y, pose.z, pose.sneaking == 1 ? Spec.SneakHeight : Spec.PlayerHeight);

    private static (double x, double y, double z) Eye(Player player) => (player.Pose.x, player.Pose.y, player.Pose.z + EyeHeightOf(player.Pose));

    private static double EyeHeightOf(WorldPresence pose) => pose.sneaking == 1 ? Spec.SneakEyeHeight : Spec.EyeHeight;

    protected override Task OnPlayerDisconnected(PlayerSession session, DisconnectReason reason)
    {
        if (_players.TryRemove(session.Id, out var player)) lock (_world) StopDig(player);
        Platform.RuntimeData.Delete(Uplink, "WorldPresence", session.Id);
        InRoom(room => room.RemovePlayer(session.Id));
        return Task.CompletedTask;
    }

    private void OnDataChanged(Platform.RuntimeDataUpdate update)
    {
        switch (update.Entity)
        {
            case "WorldCube" when update.Data.Deserialize<WorldCube>() is { } cube:
                bool changed;
                lock (_world) changed = _world.Apply(update.Op, cube);
                if (changed) Broadcast(new { type = "cube", op = update.Op, cube, remote = true });
                break;

            case "CubeInventory" when update.Data.Deserialize<CubeInventory>() is { } refill
                                      && _players.TryGetValue(refill.player_id, out var player):
                var stacks = Inventory.Parse(refill.stacks).Stacks;
                lock (_world)
                    foreach (var kind in Spec.Placeable)
                        while (player.Inventory.Count(kind) < Math.Min(Spec.StackSize, stacks.GetValueOrDefault(kind)) && player.Inventory.Give(kind)) { }
                Send(player.Session, new { type = "inventory", inventory = player.Inventory.Stacks });
                break;

            case "WorldPresence" when update.Data.Deserialize<WorldPresence>() is { } pose:
                if (update.IsDelete) _elsewhere.TryRemove(pose.player_id, out _);
                else _elsewhere[pose.player_id] = pose;
                break;

            case "WorldHit" when !update.IsDelete && update.Data.Deserialize<WorldHit>() is { } hit
                                 && Now - hit.at < 5000 && _players.TryGetValue(hit.victim, out var victim):
                lock (_world) Hurt(victim, hit.damage, (hit.kx, hit.ky), hit.strength, hit.attacker);
                Platform.RuntimeData.Delete(Uplink, "WorldHit", hit.hit_id);
                break;

            case "WorldBomb" when !update.IsDelete && update.Data.Deserialize<WorldBomb>() is { } bomb:
                lock (_world) OnBomb(bomb, owned: false);
                break;
        }
    }

    private static async Task<List<WorldCube>> LoadCubesAsync()
    {
        var cubes = new List<WorldCube>();
        string? cursor = null;
        do
        {
            var page = await Platform.Table<WorldCube>().Query().Take(200).WithCursor(cursor).ToPageAsync();
            cubes.AddRange(page.Items.Select(r => r.Fields!));
            cursor = page.NextCursor;
        } while (!string.IsNullOrEmpty(cursor));
        return cubes;
    }

    private static async Task<List<WorldBomb>> LoadBombsAsync()
    {
        var bombs = new List<WorldBomb>();
        string? cursor = null;
        do
        {
            var page = await Platform.Table<WorldBomb>().Query().Take(200).WithCursor(cursor).ToPageAsync();
            bombs.AddRange(page.Items.Select(r => r.Fields!));
            cursor = page.NextCursor;
        } while (!string.IsNullOrEmpty(cursor));
        // A bomb can have several rows (the drop function's and the servers'): the one furthest on is the bomb.
        return bombs.GroupBy(b => b.bomb_id)
            .Select(g => g.OrderByDescending(b => Bomb.Rank(b.state)).ThenByDescending(b => b.at).First())
            .Where(b => !Bomb.Over(b.state)).ToList();
    }

    private void Broadcast(object frame)
    {
        var text = JsonSerializer.Serialize(frame);
        foreach (var player in _players.Values) player.Session.TrySendText(text);
    }

    private static void Send(PlayerSession session, object frame) => session.TrySendText(JsonSerializer.Serialize(frame));

    private void InRoom(Action<WorldRoom> action)
    {
        if (_rooms.Find(RoomName) is { } room) lock (room.Lock) action(room);
    }

    private static Command? Parse(byte[] message)
    {
        try { return JsonSerializer.Deserialize<Command>(message); }
        catch (JsonException) { return null; }
    }

    private static string ServerName(string? machineId) =>
        string.IsNullOrEmpty(machineId) ? "local" : machineId[^5..].ToLowerInvariant();

    private sealed class DigState(int x, int y, int z, long startTick, int ticks)
    {
        public int X => x;
        public int Y => y;
        public int Z => z;
        public long StartTick => startTick;
        public int Ticks => ticks;
        public int Stage { get; set; } = 0;
    }

    private sealed record Player(PlayerSession Session, Inventory Inventory, WorldPresence Pose)
    {
        public bool Moved { get; set; }
        public bool Dead { get; set; }
        public bool Airborne { get; set; }
        public double Peak { get; set; }
        public long LastAttackTick { get; set; } = long.MinValue / 2;
        public long LastHurtTick { get; set; } = long.MinValue / 2;
        public DigState? Dig { get; set; }
        /// <summary>The bomb in the player's hand. A player holds one at a time and can only throw it.</summary>
        public string? Bomb { get; set; }
        /// <summary>The client can show and throw a bomb; a client that cannot is never handed one.</summary>
        public bool Throws { get; set; }
    }

    /// <summary>A bomb as this server follows it: the height of a free one, the path of one it threw.</summary>
    private sealed class LiveBomb
    {
        public required WorldBomb Record { get; init; }
        public double Z { get; set; }
        public double[] P { get; init; } = [];
        public double[] V { get; init; } = [];
        public bool Owned { get; init; }
        public int Age { get; set; }

        /// <summary>A free bomb heard late is brought down as far as it has come since it was dropped.</summary>
        public static LiveBomb Of(WorldBomb bomb, World world, bool owned, long now)
        {
            var z = bomb.z;
            if (bomb.state == CubeWorld.Server.Bomb.Free)
                for (long t = 0, ticks = Math.Clamp((now - bomb.at) / (1000 / Spec.TicksPerSecond), 0, 2000); t < ticks; t++)
                    z = CubeWorld.Server.Bomb.Descend(world, bomb.x, bomb.y, z);
            return new LiveBomb
            {
                Record = bomb, Z = z, Owned = owned && bomb.state == CubeWorld.Server.Bomb.Flying,
                P = [bomb.x, bomb.y, bomb.z], V = [bomb.vx, bomb.vy, bomb.vz],
            };
        }
    }

    private sealed record Command(string op, double x, double y, double z, double yaw, double pitch, int nx, int ny, int nz,
        string? kind, string? state, string? target, bool onGround, bool sneaking, bool sprinting);
}

public sealed class WorldPlayer : RoomPlayer;

public sealed class WorldRoom(string name) : Room<WorldPlayer, object>(name, new object(), capacity: 16);
