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
/// <remarks>
/// This file is the outline: how the server starts, what one tick does, what a player can do, what it hears from the
/// other servers. The details are next to it: <c>CubeWorldServer.Players.cs</c> (moving, digging, placing, fighting),
/// <c>CubeWorldServer.Bombs.cs</c> and <c>CubeWorldServer.Sharing.cs</c> (the regions and the world the servers share).
/// </remarks>
public sealed partial class CubeWorldServer : PlatformGameServer
{
    private const string Uplink = "";

    private readonly World _world = new();
    private readonly ConcurrentDictionary<string, Player> _players = new();
    private readonly ConcurrentDictionary<string, WorldPresence> _elsewhere = new();
    private readonly Dictionary<string, LiveBomb> _bombs = new();
    private readonly List<Change> _heard = new();
    private readonly RoomHost<WorldRoom, WorldPlayer, object> _rooms = new(name => new WorldRoom(name), tickHz: 1);
    private readonly string _server = ServerName(Environment.GetEnvironmentVariable("PLAYSERV_MACHINE_ID"));
    private int _region = -1;
    private WorldRegion[] _regions = [];
    private long _tick;

    private string Color => _region >= 0 ? World.RegionColors[_region] : "grey";

    private string RoomName => $"{Color}-{_server}";

    private static long Now => DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();

    protected override TimeSpan ReconnectGrace => TimeSpan.Zero;

    // ── the server's life ───────────────────────────────────────────────────────────────────────────

    protected override Task OnStartupAsync()
    {
        Platform.OnRuntimeDataUpdate(OnDataChanged);
        _ = Task.Run(RunAsync);
        return Task.CompletedTask;
    }

    private async Task RunAsync()
    {
        await LoadWorldAndClaimRegion();    // tries again every 5 s until the tables answer and a region is free
        StartTicking();                     // the game 20 times a second, player positions 5 times a second
        OpenRoom();
        await KeepRoomOpen();               // until the operator closes the room
        await ClearRegionAndRestart();
    }

    private async Task LoadWorldAndClaimRegion()
    {
        while (true)
        {
            try
            {
                // Subscribe before loading: a change written while the world loads arrives as an update
                // instead of being missed (applying one that the load already holds changes nothing).
                Subscribe();
                _world.Load(await LoadCubesAsync());
                await LoadBombs();
                _region = await ClaimRegionAsync();
                if (_region >= 0)
                {
                    break;
                }
            }
            catch (Exception e) { _ = Platform.Log($"world not ready, retrying in 5 s: {e.Message}"); }
            await Task.Delay(TimeSpan.FromSeconds(5));
        }
        await Platform.Log($"{RoomName}: {_world.Overrides.Count()} changed blocks loaded, {Spec.Trees.Length} oaks, {_bombs.Count} bombs, world ready");
    }

    private void StartTicking()
    {
        _ = Task.Run(ShareMovesAsync);
        _ = Task.Run(TickAsync);
    }

    private void OpenRoom() => _rooms.GetOrCreate(RoomName);

    /// <summary>The room is gone only when the platform ended it: the operator closed it, or it reached its lifetime.</summary>
    private async Task KeepRoomOpen()
    {
        while (_rooms.Find(RoomName) is { IsDisposed: false })
        {
            await SayThisServerIsAlive();
            await Task.Delay(TimeSpan.FromSeconds(5));
        }
    }

    // ── the game tick ───────────────────────────────────────────────────────────────────────────────

    private async Task TickAsync()
    {
        var next = Environment.TickCount64;
        while (true)
        {
            next += 1000 / Spec.TicksPerSecond;
            var wait = next - Environment.TickCount64;
            if (wait > 0) await Task.Delay((int)wait); else next = Environment.TickCount64;
            Tick();
        }
    }

    /// <summary>One tick: digging progresses and finishes, health regenerates, bombs come down and fly.</summary>
    private void Tick()
    {
        _tick++;

        foreach (var player in _players.Values)
        {
            Safely("tick", () =>
            {
                ProgressDigging(player);
                RegenerateHealth(player);
            });
        }

        Safely("bombs", () =>
        {
            MoveBombs();
            SendWhatOtherServersChanged();
        });
    }

    // ── players ─────────────────────────────────────────────────────────────────────────────────────

    protected override async Task OnPlayerConnected(PlayerSession session)
    {
        var name = session.DisplayName ?? session.Id;
        var inventory = await LoadInventory(session.Id);
        var player = new Player(session, inventory, Spawn(session.Id, name));

        _players[session.Id] = player;
        InRoom(room => room.AddPlayer(new WorldPlayer { Id = session.Id, DisplayName = name }));
        SendWelcome(player);
    }

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
            }
        }
        return Task.CompletedTask;
    }

    protected override Task OnPlayerDisconnected(PlayerSession session, DisconnectReason reason)
    {
        if (_players.TryRemove(session.Id, out var player)) lock (_world) StopDig(player);
        Platform.RuntimeData.Delete(Uplink, "WorldPresence", session.Id);
        InRoom(room => room.RemovePlayer(session.Id));
        return Task.CompletedTask;
    }

    // ── what the other servers and the functions changed ────────────────────────────────────────────

    private void OnDataChanged(Platform.RuntimeDataUpdate update)
    {
        switch (update.Entity)
        {
            case "WorldCube" when update.Data.Deserialize<WorldCube>() is { } cube:
                HearCube(update.Op, cube);
                break;
            case "CubeInventory" when update.Data.Deserialize<CubeInventory>() is { } refill:
                HearRefill(refill);
                break;
            case "WorldPresence" when update.Data.Deserialize<WorldPresence>() is { } pose:
                HearPresence(pose, update.IsDelete);
                break;
            case "WorldHit" when !update.IsDelete && update.Data.Deserialize<WorldHit>() is { } hit:
                HearHit(hit);
                break;
            case "WorldBomb" when !update.IsDelete && update.Data.Deserialize<WorldBomb>() is { } bomb:
                HearBomb(bomb);
                break;
        }
    }

    // ── small helpers ───────────────────────────────────────────────────────────────────────────────

    private static void Safely(string what, Action action)
    {
        try { action(); }
        catch (Exception e) { _ = Platform.Log($"{what}: {e.Message}"); }
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

    private sealed record Command(string op, double x, double y, double z, double yaw, double pitch, int nx, int ny, int nz,
        string? kind, string? state, string? target, bool onGround, bool sneaking, bool sprinting)
    {
        /// <summary>The block the command points at.</summary>
        public (int x, int y, int z) Block => ((int)Math.Floor(x), (int)Math.Floor(y), (int)Math.Floor(z));
    }
}

public sealed class WorldPlayer : RoomPlayer;

public sealed class WorldRoom(string name) : Room<WorldPlayer, object>(name, new object(), capacity: 16);
