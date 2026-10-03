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
        // The upper row first: the Unreal servers prefer the lower one, so the two kinds meet in the middle only when
        // one kind is short of servers.
        foreach (var region in Enumerable.Range(0, World.RegionColors.Length).OrderBy(r => r / World.Columns))
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
        slug = Environment.GetEnvironmentVariable("PLAYSERV_EXECUTOR_SLUG") ?? "cubeworld",
    };

    /// <summary>Every 5 s: this server still holds its region, and the players learn which regions are up.</summary>
    private async Task SayThisServerIsAlive()
    {
        Platform.RuntimeData.Write(Uplink, "WorldRegion", $"{_region}", Claim(_region));
        try { _regions = await LiveRegionsAsync(); } catch { }
        Broadcast(new { type = "regions", regions = _regions });
        Subscribe();
        ReconcileCubesNow();
        ReadBombsAgainNow();
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

    /// <summary>
    /// Our players' positions go out 20 times a second, as often as their clients send them; everyone's, ours and the
    /// others', reach the clients 10 times. The steps are fixed, so a late one does not push the next ones back. The
    /// platform stores one write of a row at a time and merges those that come meanwhile, so the other servers hear a
    /// player about every 200 ms, but always the latest position.
    /// </summary>
    private async Task ShareMovesAsync()
    {
        var next = Environment.TickCount64;
        for (var tick = 0; ; tick++)
        {
            next += 50;
            var wait = next - Environment.TickCount64;
            if (wait > 0) await Task.Delay((int)wait); else next = Environment.TickCount64;

            foreach (var player in _players.Values.Where(p => p.Moved || Now - p.Pose.seen_at > 2000))
            {
                player.Moved = false;
                player.Pose.seen_at = Now;
                Platform.RuntimeData.Write(Uplink, "WorldPresence", player.Pose.player_id, player.Pose);
            }

            if (tick % 2 == 0)
            {
                var everyone = _players.Values.Select(p => p.Pose).Concat(Others());
                Broadcast(new { type = "players", players = everyone });
            }
        }
    }

    private void HearPresence(WorldPresence pose, bool gone)
    {
        if (gone)
        {
            if (WorldPresence.DeleteTakesOut(_elsewhere.GetValueOrDefault(pose.player_id), pose)) _elsewhere.TryRemove(pose.player_id, out _);
            return;
        }
        var before = _elsewhere.GetValueOrDefault(pose.player_id);
        _elsewhere[pose.player_id] = pose;
        // A player another server hosts was hurt (a hit from here goes over as a WorldHit, a fall or a blast happens
        // there): that server tells only its own players, so this one shows ours the flash.
        if (!_players.ContainsKey(pose.player_id) && WorldPresence.WasHurt(before, pose))
            Broadcast(new { type = "hurt", player = pose.player_id, health = pose.health, by = (string?)null, kx = 0, ky = 0, strength = 0 });
    }

    // ── the blocks ──────────────────────────────────────────────────────────────────────────────────

    private static async Task<List<WorldCube>> LoadCubesAsync() =>
        (await ReadAll(() => Platform.Table<WorldCube>().Query())).Select(r => r.Fields!).ToList();

    /// <summary>
    /// Every row a query finds, 200 a page, each page the rows after the last record id of the page before (PSV-3014).
    /// The platform's cursor skips a count of rows in updated_at order, newest first, so a row written or deleted while
    /// the read ran moved others past a page boundary, and they were never returned: on dev on 2026-10-02 two Unreal
    /// servers lost 39 and 288 blocks in one read that way. A record id never changes, and the platform filters and sorts
    /// it with the same collation, so a boundary stays where it was. <paramref name="query"/> makes a fresh query for
    /// each page, since a query adds every filter it is given to the ones it has.
    /// </summary>
    private static async Task<List<Record<T>>> ReadAll<T>(Func<RecordQuery<T>> query) where T : class
    {
        var rows = new List<Record<T>>();
        string? after = null;
        while (true)
        {
            var next = query().WithSort("id", "asc").Take(200);
            if (after is not null) next.WithFilters(new RecordFilterTerm("id", "gt", after));
            var page = await next.ToPageAsync();
            rows.AddRange(page.Items);
            if (string.IsNullOrEmpty(page.NextCursor)) return rows;
            // A page that names no last row, or the one the page before ended on, cannot be followed: the read fails
            // rather than come back short, since a caller takes a short read for the whole table.
            var last = page.Items.Count > 0 ? page.Items[^1].Id : null;
            if (string.IsNullOrEmpty(last) || last == after)
                throw new InvalidOperationException("a page said more rows follow, but it did not end on a new record id");
            after = last;
        }
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

    /// <summary>
    /// Every 30 s the whole table is read back and what this server missed is applied: the platform's pushes to a game
    /// server are lost when the uplink drops for a moment, and nothing replays them, so a block placed or deleted then
    /// stayed wrong here until the process restarted. A block that changed here since the read began is left as it is;
    /// a row is taken only when it is newer than what this server holds;
    /// a block whose row is gone goes back to the terrain (World.Forget), as ACubeWorldGameMode::ReconcileCubes does
    /// on the Unreal servers.
    /// </summary>
    private async Task ReconcileCubesAsync()
    {
        var started = Now;
        long asOf;
        lock (_world) asOf = _world.Version;
        var rows = (await ReadAll(() => Platform.Table<WorldCube>().Query())).Select(r => r.Fields!).ToList();
        int taken = 0, gone;
        lock (_world)
        {
            foreach (var cube in rows)
            {
                if (_world.ChangedSince(cube.key, asOf) || !_world.Reconcile(cube, _server, Now)) continue;
                _heard.Add(new Change("upsert", cube));
                taken++;
            }
            var forgotten = _world.Forget(rows.Select(c => c.key).ToHashSet(), asOf, _server, Now);
            foreach (var cube in forgotten) _heard.Add(new Change("delete", cube));
            gone = forgotten.Count;
        }
        _cubesReadAt = started;
        if (taken + gone > 0) _ = Platform.Log($"{RoomName}: blocks read back from the table: {taken} taken, {gone} back to the terrain");
    }

    private void ReconcileCubesNow()
    {
        if (Now - _cubesReadAt < 30_000 || Interlocked.Exchange(ref _reconciling, 1) == 1) return;
        _ = Task.Run(async () =>
        {
            try { await ReconcileCubesAsync(); }
            catch (Exception e) { _ = Platform.Log($"{RoomName}: blocks not read back, again in 30 s: {e.Message}"); }
            finally { Interlocked.Exchange(ref _reconciling, 0); }
        });
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

    // ── when the operator closes the room ───────────────────────────────────────────────────────────

    /// <summary>
    /// The room was closed. The bombs over its region go up in smoke and the process ends; Docker starts it again on
    /// the same machine, where it claims its region again and opens the room fresh. The blocks stay as they are:
    /// the world goes back to its default state only through the cubeworld-reset function.
    /// </summary>
    private async Task Restart()
    {
        try
        {
            await Platform.Log($"{RoomName} was closed: restarting");
            FizzleBombsOverRegion();
        }
        catch (Exception e) { await Platform.Log($"{RoomName}: bombs over region {_region} not fizzled: {e.Message}"); }

        // The platform has its answer and the players their close code already; this gives the smoke and the logs
        // time to leave before the process does.
        await Task.Delay(TimeSpan.FromSeconds(3));
        Environment.Exit(0);
    }
}
