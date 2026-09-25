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
                _world.Load(await LoadCubesAsync());
                _region = await ClaimRegionAsync();
                if (_region >= 0) break;
            }
            catch { }
            await Task.Delay(TimeSpan.FromSeconds(5));
        }
        await Platform.Log($"{RoomName}: {_world.Overrides.Count()} changed blocks loaded, {Spec.Trees.Length} oaks, world ready");
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
            Platform.RuntimeData.Subscribe(Uplink, "WorldCube", "field:key");
            Platform.RuntimeData.Subscribe(Uplink, "CubeInventory", "field:player_id");
            Platform.RuntimeData.Subscribe(Uplink, "WorldPresence", "field:player_id");
            await Task.Delay(TimeSpan.FromSeconds(5));
        }
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

    /// <summary>The 20 Hz game tick: digging progresses and finishes, health regenerates.</summary>
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
        }
    }

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
        lock (_world) world = _world.Overrides.ToArray();
        Send(session, new
        {
            type = "welcome", server = _server, color = Color, region = _region, regions = _regions, you = pose,
            width = World.Width, depth = World.Depth, regionSize = World.RegionSize, minZ = World.MinZ, maxZ = World.MaxZ,
            layers = Spec.Layers.Select(l => new { l.z, l.kind }), trees = Spec.Trees.Select(t => new { t.x, t.y }),
            blocks = Spec.Blocks.Select(b => new { kind = b.Kind, b.Hardness, b.NeedsTool, b.Transparent, b.Gravity, b.Drop, breakTicks = b.Breakable ? b.BreakTicks : -1 }),
            hotbar = Spec.Placeable, world, inventory = inventory.Stacks, tick = _tick,
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

    private void Attack(Player player, Command command)
    {
        if (player.Dead || command.target is null || !_players.TryGetValue(command.target, out var victim) || victim == player || victim.Dead) return;

        // A hand recharges in 5 ticks; a hit before that is weaker: 20 % plus 80 % of the charge squared.
        var charge = Math.Min(1.0, (_tick - player.LastAttackTick) / (double)Spec.FistChargeTicks);
        player.LastAttackTick = _tick;

        var (ex, ey, ez) = Eye(player);
        if (World.DistanceToHitbox(ex, ey, ez, HitboxOf(victim.Pose)) > Spec.EntityReach + Spec.ReachTolerance) return;

        var damage = Spec.FistDamage * (0.2 + 0.8 * charge * charge);
        double dx = victim.Pose.x - player.Pose.x, dy = victim.Pose.y - player.Pose.y;
        var length = Math.Sqrt(dx * dx + dy * dy);
        if (length < 1e-4) { dx = -Math.Sin(player.Pose.yaw); dy = Math.Cos(player.Pose.yaw); length = 1; }
        var strength = Spec.Knockback + (player.Pose.sprinting == 1 ? Spec.SprintKnockback : 0);
        Hurt(victim, damage, (dx / length, dy / length), strength, player);
    }

    private void Hurt(Player victim, double damage, (double x, double y)? direction, double strength, Player? by)
    {
        if (victim.Dead || _tick - victim.LastHurtTick < Spec.InvulnerabilityTicks) return;
        victim.LastHurtTick = _tick;
        victim.Pose.health = Math.Max(0, victim.Pose.health - damage);
        victim.Moved = true;
        Broadcast(new
        {
            type = "hurt", player = victim.Pose.player_id, health = victim.Pose.health, by = by?.Pose.player_id,
            kx = direction?.x * strength ?? 0, ky = direction?.y * strength ?? 0, strength = direction is null ? 0 : strength,
        });

        if (victim.Pose.health > 0) return;
        victim.Dead = true;
        StopDig(victim);
        Broadcast(new { type = "death", player = victim.Pose.player_id, by = by?.Pose.player_id });
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

    private static (double x, double y, double z) Eye(Player player) =>
        (player.Pose.x, player.Pose.y, player.Pose.z + (player.Pose.sneaking == 1 ? Spec.SneakEyeHeight : Spec.EyeHeight));

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
    }

    private sealed record Command(string op, double x, double y, double z, double yaw, double pitch, int nx, int ny, int nz,
        string? kind, string? state, string? target, bool onGround, bool sneaking, bool sprinting);
}

public sealed class WorldPlayer : RoomPlayer;

public sealed class WorldRoom(string name) : Room<WorldPlayer, object>(name, new object(), capacity: 16);
