using PlayServ.Sdk;
using PlayServ.Sdk.Data;

namespace CubeWorld.Server;

/// <summary>
/// One world, several servers: each holds one region, writes what changes there to platform data and hears what the
/// others write.
/// </summary>
public sealed partial class CubeWorldServer
{
    // ── the region this server holds ────────────────────────────────────────────────────────────────

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

    /// <summary>Every 5 s: this server still holds its region, the players learn which regions are up, and the epoch is checked.</summary>
    private async Task SayThisServerIsAlive()
    {
        Platform.RuntimeData.Write(Uplink, "WorldRegion", $"{_region}", Claim(_region));
        try { _regions = await LiveRegionsAsync(); } catch { }
        Broadcast(new { type = "regions", regions = _regions });
        Subscribe();
        await WatchEpoch();
    }

    // ── the world's epoch: a reset rewrote the table, every server starts over from it ─────────────

    private static async Task<long> ReadEpoch() =>
        (await Platform.Table<WorldEpoch>().FindByAsync(e => e.name, "world"))?.Fields?.epoch ?? 0;

    private async Task WatchEpoch()
    {
        try
        {
            var epoch = await ReadEpoch();
            if (epoch == _epoch) return;
            await Platform.Log($"{RoomName}: the world was reset (epoch {_epoch} → {epoch}), reopening the room from the table");
            _newEpoch = true;
        }
        catch (Exception e) { _ = Platform.Log($"{RoomName}: epoch not read: {e.Message}"); }
    }

    /// <summary>
    /// A player asked for the world to start over. The reset function does it in bulk: every changed block cleared
    /// in one call, the default blocks inserted 200 a call, the epoch bumped; then every server, this one included,
    /// sees the new epoch and reopens its room from the table.
    /// </summary>
    private void RequestReset(Player player)
    {
        Broadcast(new { type = "resetting", by = player.Pose.player_id });
        _ = Task.Run(async () =>
        {
            try { await CallReset($"{player.Pose.name} on {RoomName}"); }
            catch (Exception e) { _ = Platform.Log($"{RoomName}: reset refused: {e.Message}"); }
        });
    }

    private static async Task CallReset(string by)
    {
        var answer = await Platform.CallFunction<System.Text.Json.JsonElement>("cubeworld-reset", new { By = by });
        await Platform.Log($"reset by {by}: {answer}");
    }

    private static async Task<WorldRegion[]> LiveRegionsAsync()
    {
        var rows = await Platform.Table<WorldRegion>().Query().ToListAsync();
        return rows.Select(r => r.Fields!).Where(r => Now - r.seen_at < 30_000).ToArray();
    }

    private static void Subscribe()
    {
        Platform.RuntimeData.Subscribe(Uplink, "WorldCube", "field:key");
        Platform.RuntimeData.Subscribe(Uplink, "CubeInventory", "field:player_id");
        Platform.RuntimeData.Subscribe(Uplink, "WorldPresence", "field:player_id");
        Platform.RuntimeData.Subscribe(Uplink, "WorldHit", "field:hit_id");
        Platform.RuntimeData.Subscribe(Uplink, "WorldBomb", "field:bomb_id");
    }

    // ── where the players are ───────────────────────────────────────────────────────────────────────

    /// <summary>Our players' positions go out 5 times a second; everyone's, ours and the others', reach the clients 10 times.</summary>
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

            var everyone = _players.Values.Select(p => p.Pose).Concat(Others());
            Broadcast(new { type = "players", players = everyone });
        }
    }

    private void HearPresence(WorldPresence pose, bool gone)
    {
        if (gone) _elsewhere.TryRemove(pose.player_id, out _);
        else _elsewhere[pose.player_id] = pose;
    }

    // ── the blocks ──────────────────────────────────────────────────────────────────────────────────

    private static async Task<List<WorldCube>> LoadCubesAsync() =>
        (await ReadAll(Platform.Table<WorldCube>().Query())).Select(r => r.Fields!).ToList();

    /// <summary>Every row a query finds, 200 a page.</summary>
    private static async Task<List<Record<T>>> ReadAll<T>(RecordQuery<T> query) where T : class
    {
        var rows = new List<Record<T>>();
        string? cursor = null;
        do
        {
            var page = await query.Take(200).WithCursor(cursor).ToPageAsync();
            rows.AddRange(page.Items);
            cursor = page.NextCursor;
        } while (!string.IsNullOrEmpty(cursor));
        return rows;
    }

    /// <summary>A block changed here: the other servers hear it through platform data, our players see it now.</summary>
    private void Publish(WorldUpdate update)
    {
        foreach (var (op, cube) in update.Changes)
        {
            if (op == "delete") Platform.RuntimeData.Delete(Uplink, "WorldCube", cube.key);
            else Platform.RuntimeData.Write(Uplink, "WorldCube", cube.key, cube);
        }
        BroadcastCubes(update.Falls, update.Changes, remote: false);
    }

    private void HearCube(string op, WorldCube cube)
    {
        lock (_world)
        {
            if (_world.Apply(op, cube)) _heard.Add(new Change(op, cube));
        }
    }

    /// <summary>What the other servers changed this tick goes out together (a blast elsewhere is a hundred blocks).</summary>
    private void SendWhatOtherServersChanged()
    {
        lock (_world)
        {
            if (_heard.Count == 0) return;
            BroadcastCubes([], _heard.ToList(), remote: true);
            _heard.Clear();
        }
    }

    /// <summary>
    /// A player's socket holds at most 64 frames queued and is cut the moment one more is sent, so blocks that change
    /// together go out together: one "cubes" frame.
    /// </summary>
    private void BroadcastCubes(IReadOnlyList<Fall> falls, IReadOnlyList<Change> changes, bool remote)
    {
        if (falls.Count == 0 && changes.Count == 0) return;
        Broadcast(new
        {
            type = "cubes", remote,
            falls = falls.Select(f => new { kind = f.Kind, x = f.X, y = f.Y, fromZ = f.FromZ, toZ = f.ToZ }),
            changes = changes.Select(c => new { op = c.Op, cube = c.Cube }),
        });
    }

    // ── when the operator closes the room, or the world was reset ───────────────────────────────────

    /// <summary>
    /// The operator closed the room: the whole world goes back to its default state, in bulk, through the reset
    /// function; the bombs over this region go up in smoke; the process ends. Docker starts it again on the same
    /// machine, and it, like every other server that sees the new epoch, reopens its room from the table.
    /// </summary>
    private async Task ResetWorldAndRestart()
    {
        try
        {
            await Platform.Log($"{RoomName} was closed: resetting the world and restarting");
            FizzleBombsOverRegion();
            await CallReset($"operator closed {RoomName}");
        }
        catch (Exception e) { await Platform.Log($"{RoomName}: the world was not reset: {e.Message}"); }
        await Restart();
    }

    /// <summary>The world was reset under this room: the room closes and the process starts over from the table.</summary>
    private async Task RestartForTheNewWorld()
    {
        Broadcast(new { type = "reset", epoch = _epoch });
        FizzleBombsOverRegion();
        await Restart();
    }

    // The platform has its answer and the players their close code already; this gives the smoke and the logs
    // time to leave before the process does.
    private static async Task Restart()
    {
        await Task.Delay(TimeSpan.FromSeconds(3));
        Environment.Exit(0);
    }
}
