using PlayServ.Sdk;
using PlayServ.Sdk.Data;
using BombRows = System.Linq.IGrouping<string, PlayServ.Sdk.Data.Record<CubeWorld.Drop.WorldBomb>>;

namespace CubeWorld.Drop;

[EntityName("WorldBomb")]
public sealed class WorldBomb
{
    public string bomb_id { get; set; } = "";
    public string state { get; set; } = "";
    /// Who holds it or threw it; the drop's own rows hold none.
    public string? holder { get; set; }
    public double x { get; set; }
    public double y { get; set; }
    public double z { get; set; }
    public double? vx { get; set; }   // a thrown bomb's motion; a dropped one writes 0, older rows may hold none
    public double? vy { get; set; }
    public double? vz { get; set; }
    public long dropped_at { get; set; }
    public long at { get; set; }
}

/// Only what this function reads of a player's presence: when a server last said the player was there.
[EntityName("WorldPresence")]
public sealed class WorldPresence
{
    public string player_id { get; set; } = "";
    public long seen_at { get; set; }
}

/// Only what this function reads of a region: which one it is and when its server last said it holds it.
[EntityName("WorldRegion")]
public sealed class WorldRegion
{
    public string region { get; set; } = "";
    public long seen_at { get; set; }
}

/// Every minute: two bombs over every region a server holds, that is over every room. At most three lie free in a region.
public sealed class DropBombs : PlatformFunction<object>
{
    public const int BombsPerRegion = 2, MaxFreeBombs = 3;
    private const int PageSize = 200;

    /// The world's regions, as CubeWorld.Server's World lays them out: 24 × 24 blocks, three to a row, two rows.
    public const int RegionSize = 24, Columns = 3, Regions = 6;

    /// A region whose server has not said it holds it for this long has no room: the servers read it the same way.
    public const long RegionGoneMs = 30_000;

    /// A held bomb whose holder no server has seen for this long: the player left with it, and it goes up in smoke.
    public const long HolderGoneMs = 60_000;

    /// A thrown bomb flies 200 ticks (10 s) at most: one still flying after this long lost its thrower's server.
    public const long FlightLostMs = 30_000;

    /// A presence row can be missing for a moment while a player crosses from one server to the next, so a holder
    /// with no row at all is looked up once more after this long before their bomb goes up in smoke.
    private static readonly TimeSpan CrossingGrace = TimeSpan.FromSeconds(5);

    private ITable<WorldBomb> WorldBombs => Platform.Table<WorldBomb>();
    private ITable<WorldPresence> Presences => Platform.Table<WorldPresence>();
    private ITable<WorldRegion> WorldRegions => Platform.Table<WorldRegion>();

    private static long Now => DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();

    protected override async Task<FunctionResponse> HandleAsync(object _, CancellationToken ct)
    {
        var bombs = await LoadBombs(ct);

        await ForgetFinishedBombs(bombs, ct);
        await FizzleLostBombs(bombs, ct);

        var regions = LiveRegions((await WorldRegions.Query().ToListAsync(ct)).Select(r => r.Fields).OfType<WorldRegion>(), Now);
        foreach (var region in regions)
        {
            foreach (var old in OldestFreeToFizzle(bombs.Select(b => Furthest(b.Select(r => r.Fields!))), region))
            {
                await WorldBombs.CreateAsync(Bomb(old.bomb_id, "fizzled", null, old.x, old.y, old.dropped_at, Now), ct);
            }
            foreach (var (x, y) in Spots(region, Random.Shared))
            {
                var now = Now;
                await WorldBombs.CreateAsync(Bomb($"drop-{now}-{region}-{x}-{y}", "free", null, x, y, now, now), ct);
            }
        }

        return FunctionResponse.Json(new { ok = true, regions, dropped = regions.Count * BombsPerRegion });
    }

    /// Every bomb with all its rows (this function writes the first one, the servers add theirs as it is picked up and
    /// thrown), a page at a time: the table holds more than one page of rows.
    private async Task<List<BombRows>> LoadBombs(CancellationToken ct)
    {
        var rows = new List<Record<WorldBomb>>();
        string? cursor = null;
        do
        {
            var page = await WorldBombs.Query().OrderBy(b => b.bomb_id).Take(PageSize).WithCursor(cursor).ToPageAsync(ct);
            rows.AddRange(page.Items.Where(r => r.Fields is not null));
            cursor = page.NextCursor;
        } while (!string.IsNullOrEmpty(cursor));
        return rows.GroupBy(r => r.Fields!.bomb_id).ToList();
    }

    /// A bomb that went off over two minutes ago: forget it.
    private async Task ForgetFinishedBombs(List<BombRows> bombs, CancellationToken ct)
    {
        foreach (var bomb in bombs.Where(b => WentOffLongAgo(b.Select(r => r.Fields!), Now)))
        {
            foreach (var row in bomb)
            {
                await WorldBombs.DeleteAsync(row.Id, ct);
            }
        }
    }

    /// <summary>
    /// A bomb in the hand of a player who left the game, or still flying long after any throw would have landed: it
    /// goes up in smoke. A held bomb is kept through a border crossing (the next server hands it back), so only a
    /// holder no server has seen for a minute counts as gone, and a holder with no presence row at all only when the
    /// row is still missing <see cref="CrossingGrace"/> later.
    /// </summary>
    private async Task FizzleLostBombs(List<BombRows> bombs, CancellationToken ct)
    {
        var lost = new List<WorldBomb>();
        var missing = new List<WorldBomb>();
        foreach (var bomb in bombs)
        {
            var now = Now;
            var furthest = Furthest(bomb.Select(r => r.Fields!));
            if (furthest.state == "flying" && now - furthest.at > FlightLostMs)
            {
                lost.Add(furthest);
            }
            else if (furthest.state == "held" && now - furthest.at > HolderGoneMs)
            {
                var seen = await LastSeen(furthest.holder, ct);
                if (seen is null) missing.Add(furthest);
                else if (HolderGone(seen, now)) lost.Add(furthest);
            }
        }

        if (missing.Count > 0)
        {
            await Task.Delay(CrossingGrace, ct);
            foreach (var bomb in missing)
            {
                if (HolderGone(await LastSeen(bomb.holder, ct), Now)) lost.Add(bomb);
            }
        }

        foreach (var bomb in lost)
        {
            await WorldBombs.CreateAsync(Bomb(bomb.bomb_id, "fizzled", bomb.holder, bomb.x, bomb.y, bomb.dropped_at, Now), ct);
        }
    }

    private async Task<long?> LastSeen(string? playerId, CancellationToken ct)
    {
        if (string.IsNullOrEmpty(playerId))
        {
            return null;
        }

        var row = await Presences.FindByAsync(p => p.player_id, playerId, ct);
        return row?.Fields?.seen_at;
    }

    /// <summary>The regions a server holds now, each once: a room is up there.</summary>
    public static List<int> LiveRegions(IEnumerable<WorldRegion> rows, long now) =>
        rows.Where(r => now - r.seen_at < RegionGoneMs)
            .Select(r => int.TryParse(r.region, out var region) ? region : -1)
            .Where(region => region is >= 0 and < Regions)
            .Distinct()
            .Order()
            .ToList();

    public static int RegionOf(double x, double y) => (int)Math.Floor(y / RegionSize) * Columns + (int)Math.Floor(x / RegionSize);

    /// <summary>
    /// The oldest free bombs of a region that go up in smoke so that the new ones leave at most
    /// <see cref="MaxFreeBombs"/> lying there.
    /// </summary>
    public static IEnumerable<WorldBomb> OldestFreeToFizzle(IEnumerable<WorldBomb> bombs, int region)
    {
        var free = bombs.Where(b => b.state == "free" && RegionOf(b.x, b.y) == region).OrderBy(b => b.dropped_at).ToList();
        return free.Take(Math.Max(0, free.Count - (MaxFreeBombs - BombsPerRegion)));
    }

    /// <summary>Where a region's new bombs come down: different blocks, a block in from its border.</summary>
    public static List<(int X, int Y)> Spots(int region, Random random)
    {
        var (x0, y0) = (region % Columns * RegionSize, region / Columns * RegionSize);
        var spots = new List<(int X, int Y)>();
        while (spots.Count < BombsPerRegion)
        {
            var spot = (random.Next(x0 + 1, x0 + RegionSize), random.Next(y0 + 1, y0 + RegionSize));
            if (!spots.Contains(spot)) spots.Add(spot);
        }
        return spots;
    }

    /// <summary>Free, held, flying, then over (exploded or fizzled): a bomb only ever moves forward through these.</summary>
    public static int Rank(string state) => state switch { "free" => 0, "held" => 1, "flying" => 2, _ => 3 };

    /// <summary>The row furthest on is the bomb as it stands (the latest of them when two are as far).</summary>
    public static WorldBomb Furthest(IEnumerable<WorldBomb> rows) =>
        rows.OrderByDescending(r => Rank(r.state)).ThenByDescending(r => r.at).First();

    /// <summary>The holder is gone: no presence row, or none written for <see cref="HolderGoneMs"/>.</summary>
    public static bool HolderGone(long? lastSeen, long now) => lastSeen is not { } seen || now - seen > HolderGoneMs;

    public static bool WentOffLongAgo(IEnumerable<WorldBomb> rows, long now) =>
        rows.Any(r => r.state is "exploded" or "fizzled" && now - r.at > 120_000);

    /// <summary>
    /// A row as this function writes it. <paramref name="at"/> is taken right before the write: a row stamped earlier
    /// lands behind what the Unreal servers have already heard (they hear rows newer than the last one they saw).
    /// </summary>
    private static WorldBomb Bomb(string id, string state, string? holder, double x, double y, long droppedAt, long at) =>
        new() { bomb_id = id, state = state, holder = holder, x = x, y = y, z = 32, vx = 0, vy = 0, vz = 0, dropped_at = droppedAt, at = at };
}
