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

/// Every minute: 4 bombs, one every 15 seconds. At most 5 lie free.
public sealed class DropBombs : PlatformFunction<object>
{
    private const int BombsPerRun = 4, MaxFreeBombs = 5, PageSize = 200;

    /// A held bomb whose holder no server has seen for this long: the player left with it, and it goes up in smoke.
    public const long HolderGoneMs = 60_000;

    /// A thrown bomb flies 200 ticks (10 s) at most: one still flying after this long lost its thrower's server.
    public const long FlightLostMs = 30_000;

    private ITable<WorldBomb> WorldBombs => Platform.Table<WorldBomb>();
    private ITable<WorldPresence> Presences => Platform.Table<WorldPresence>();

    private static long Now => DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();

    protected override async Task<FunctionResponse> HandleAsync(object _, CancellationToken ct)
    {
        var suspects = new HashSet<string>();
        for (var i = 0; i < BombsPerRun; i++)
        {
            if (i > 0)
            {
                await Task.Delay(TimeSpan.FromSeconds(15), ct);
            }

            var bombs = await LoadBombs(ct);

            await ForgetFinishedBombs(bombs, ct);
            await FizzleLostBombs(bombs, suspects, ct);
            await FizzleOldestFreeBombs(bombs, ct);
            await DropNewBomb(ct);
        }

        return FunctionResponse.Json(new { ok = true });
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
    /// holder no server has seen for a minute counts as gone, and only when they are still gone 15 s later (the next
    /// drop of this run): a presence row can be missing for a moment while a player crosses from one server to the next.
    /// </summary>
    private async Task FizzleLostBombs(List<BombRows> bombs, HashSet<string> suspects, CancellationToken ct)
    {
        foreach (var bomb in bombs)
        {
            var now = Now;
            var furthest = Furthest(bomb.Select(r => r.Fields!));
            var lost = furthest.state switch
            {
                "flying" => now - furthest.at > FlightLostMs,
                "held" => now - furthest.at > HolderGoneMs && HolderGone(await LastSeen(furthest.holder, ct), now),
                _ => false,
            };
            if (!lost)
            {
                suspects.Remove(furthest.bomb_id);
                continue;
            }
            if (furthest.state == "held" && suspects.Add(furthest.bomb_id))
            {
                continue;
            }

            await WorldBombs.CreateAsync(Bomb(furthest.bomb_id, "fizzled", furthest.holder, furthest.x, furthest.y, furthest.dropped_at, Now), ct);
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

    /// Five bombs already free: the oldest goes up in smoke, to make room for the new one.
    private async Task FizzleOldestFreeBombs(List<BombRows> bombs, CancellationToken ct)
    {
        var free = bombs
            .Select(b => Furthest(b.Select(r => r.Fields!)))
            .Where(b => b.state == "free")
            .OrderBy(b => b.dropped_at)
            .ToList();

        foreach (var old in free.Take(free.Count - (MaxFreeBombs - 1)))
        {
            await WorldBombs.CreateAsync(Bomb(old.bomb_id, "fizzled", null, old.x, old.y, old.dropped_at, Now), ct);
        }
    }

    /// A new bomb somewhere over the world.
    private async Task DropNewBomb(CancellationToken ct)
    {
        var now = Now;
        await WorldBombs.CreateAsync(Bomb($"drop-{now}", "free", null, Random.Shared.Next(1, 71), Random.Shared.Next(1, 47), now, now), ct);
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
