using System.Collections.Concurrent;
using System.Text.Json;
using PlayServ.Sdk;
using PlayServ.Sdk.Data;
using PlayServ.Sdk.Rooms;

namespace CubeWorld.Server;

public sealed class CubeWorldServer : PlatformGameServer
{
    private const int MaxCubes = 10;
    private const string Uplink = "";

    private readonly World _world = new();
    private readonly ConcurrentDictionary<string, Player> _players = new();
    private readonly ConcurrentDictionary<string, WorldPresence> _elsewhere = new();
    private readonly RoomHost<WorldRoom, WorldPlayer, object> _rooms = new(name => new WorldRoom(name), tickHz: 1);
    private readonly string _server = ServerName(Environment.GetEnvironmentVariable("PLAYSERV_MACHINE_ID"));
    private int _region = -1;
    private WorldRegion[] _regions = [];

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
        await Platform.Log($"{RoomName}: {_world.Cubes.Count()} cubes loaded, world ready");
        _ = Task.Run(ShareMovesAsync);

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

    protected override async Task OnPlayerConnected(PlayerSession session)
    {
        var name = session.DisplayName ?? session.Id;
        var saved = await Platform.Table<CubeInventory>().FindByAsync(i => i.player_id, session.Id);
        var inventory = saved?.Fields ?? new CubeInventory { player_id = session.Id, cubes = MaxCubes };
        Platform.RuntimeData.Write(Uplink, "CubeInventory", session.Id, inventory);

        var pose = new WorldPresence { player_id = session.Id, name = name, server = _server, color = Color, x = (_region + 0.5) * World.RegionSize, y = World.Depth / 2.0 };
        _players[session.Id] = new Player(session, inventory, pose);
        InRoom(room => room.AddPlayer(new WorldPlayer { Id = session.Id, DisplayName = name }));

        WorldCube[] cubes;
        lock (_world) cubes = _world.Cubes.ToArray();
        Send(session, new
        {
            type = "welcome", server = _server, color = Color, region = _region, regions = _regions, you = pose,
            width = World.Width, depth = World.Depth, regionSize = World.RegionSize, height = World.Height,
            kinds = World.Kinds, world = cubes, inventory = inventory.cubes,
        });
    }

    protected override Task OnPlayerMessage(PlayerSession session, byte[] message)
    {
        if (!_players.TryGetValue(session.Id, out var player) || Parse(message) is not { } command) return Task.CompletedTask;

        if (command.op == "move")
        {
            player.Pose.x = Math.Clamp(command.x, 0, World.Width);
            player.Pose.y = Math.Clamp(command.y, 0, World.Depth);
            player.Pose.z = Math.Clamp(command.z, 0, World.Height + 4);
            player.Pose.yaw = command.yaw;
            player.Moved = true;
        }

        InRoom(room => room.MarkActive(room.GetPlayer(session.Id)));
        if (command.op == "move") return Task.CompletedTask;
        int x = (int)command.x, y = (int)command.y;
        WorldCube? cube = null;
        lock (_world)
        {
            if (command.op == "place" && player.Inventory.cubes > 0)
            {
                cube = _world.Place(x, y, command.kind ?? "stone", session.Id, _server);
                if (cube is not null) player.Inventory.cubes--;
            }
            else if (command.op == "break")
            {
                cube = _world.Break(x, y);
            }
        }

        if (cube is null)
        {
            Send(session, new { type = "refused", command.op, inventory = player.Inventory.cubes });
            return Task.CompletedTask;
        }

        if (command.op == "place")
        {
            Platform.RuntimeData.Write(Uplink, "WorldCube", cube.key, cube);
            Platform.RuntimeData.Write(Uplink, "CubeInventory", session.Id, player.Inventory);
            Send(session, new { type = "inventory", inventory = player.Inventory.cubes });
        }
        else
        {
            Platform.RuntimeData.Delete(Uplink, "WorldCube", cube.key);
        }
        Broadcast(new { type = "cube", op = command.op == "place" ? "upsert" : "delete", cube, remote = false });
        return Task.CompletedTask;
    }

    protected override Task OnPlayerDisconnected(PlayerSession session, DisconnectReason reason)
    {
        _players.TryRemove(session.Id, out _);
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
                lock (_world) player.Inventory.cubes = Math.Min(refill.cubes, MaxCubes);
                Send(player.Session, new { type = "inventory", inventory = player.Inventory.cubes });
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

    private sealed record Player(PlayerSession Session, CubeInventory Inventory, WorldPresence Pose)
    {
        public bool Moved { get; set; }
    }

    private sealed record Command(string op, double x, double y, double z, double yaw, string? kind);
}

public sealed class WorldPlayer : RoomPlayer;

public sealed class WorldRoom(string name) : Room<WorldPlayer, object>(name, new object(), capacity: 16);
