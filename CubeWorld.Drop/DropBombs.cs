using PlayServ.Sdk;
using PlayServ.Sdk.Data;

namespace CubeWorld.Drop;

/// <summary>The bomb as CubeWorld.Server/Bomb.cs describes it; only the fields this function writes matter here.</summary>
[EntityName("WorldBomb")]
public sealed class WorldBomb
{
    public string bomb_id { get; set; } = "";
    public string state { get; set; } = "";
    public string holder { get; set; } = "";
    public double x { get; set; }
    public double y { get; set; }
    public double z { get; set; }
    public double vx { get; set; }
    public double vy { get; set; }
    public double vz { get; set; }
    public long dropped_at { get; set; }
    public long at { get; set; }
}

public sealed record DropRequest(int? Minutes);

/// <summary>
/// Drops a bomb on a parachute every 15 seconds, on the clock's quarter minutes. At most five bombs lie free in the
/// world: before a sixth comes, the oldest goes up in smoke.
/// </summary>
/// <remarks>
/// A cron fires once a minute at most and starts some 30 seconds late, and the platform skips a fire while the last
/// one still runs. So one fire runs for half an hour, dropping on every quarter minute, and the fires in between are
/// skipped; the next one takes over when it ends. A drop is named by its quarter minute and placed by a random seeded
/// with it, so two fires that ever overlap write the very same bomb.
/// </remarks>
/// <remarks>
/// bomb_id is not the entity's primary key, so one bomb can have several rows: the servers write theirs through
/// runtime data, keyed by bomb_id, and this function creates its own. A bomb only moves forward through its states,
/// so the row furthest on is the bomb; this function only ever adds rows, never updates one.
/// </remarks>
public sealed class DropBombs : PlatformFunction<DropRequest>
{
    public const int MaxFree = 5, Width = 72, Depth = 24, RunMinutes = 30;
    public const double DropHeight = 32;
    public const long Interval = 15_000;

    // A thrown bomb whose server went away never lands; it goes after half a minute. Finished bombs are swept
    // after two: every server has heard them by then.
    private const long StaleFlight = 30_000, Sweep = 120_000;

    protected override async Task<FunctionResponse> HandleAsync(DropRequest body, CancellationToken ct)
    {
        var until = DateTimeOffset.UtcNow.AddMinutes(Math.Clamp(body.Minutes ?? RunMinutes, 1, RunMinutes)).ToUnixTimeMilliseconds();
        var table = Platform.Table<WorldBomb>();
        int dropped = 0, fizzled = 0, swept = 0;

        for (var slot = NextSlot(DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()); slot <= until; slot += Interval)
        {
            var wait = slot - DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
            if (wait > 0) await Task.Delay(TimeSpan.FromMilliseconds(wait), ct);
            var rows = await LoadAsync(table, ct);
            var plan = Plan(rows.Select(r => r.Fields!), slot);

            foreach (var bomb in plan.Fizzle)
            {
                await table.CreateAsync(bomb, ct);
                fizzled++;
            }
            foreach (var row in rows.Where(r => plan.Sweep.Contains(r.Fields!.bomb_id)))
            {
                try { await table.DeleteAsync(row.Id, ct); swept++; }
                catch (ApiException) { }
            }
            await table.CreateAsync(plan.Drop, ct);
            dropped++;
            await Platform.Log($"dropped bomb {plan.Drop.bomb_id} over ({plan.Drop.x:0.0}, {plan.Drop.y:0.0}), {plan.Free} free", ct: ct);
        }

        return FunctionResponse.Json(new { dropped, fizzled, swept });
    }

    /// <summary>What one drop does: which bombs go up in smoke, which finished ones are swept, and the new bomb.</summary>
    /// <summary>The first quarter minute at or after <paramref name="now"/>, Unix milliseconds.</summary>
    internal static long NextSlot(long now) => (now + Interval - 1) / Interval * Interval;

    internal static (List<WorldBomb> Fizzle, HashSet<string> Sweep, WorldBomb Drop, int Free) Plan(IEnumerable<WorldBomb> bombs, long now)
    {
        var random = new Random((int)(now / Interval % int.MaxValue));
        var all = Latest(bombs);
        var fizzle = new List<WorldBomb>();

        var free = all.Where(b => b.state == "free").OrderBy(b => b.dropped_at).ToList();
        var oldest = free.Take(Math.Max(0, free.Count - (MaxFree - 1))).ToList();
        fizzle.AddRange(oldest.Select(b => Fizzled(b, now)));
        foreach (var stale in all.Where(b => b.state == "flying" && now - b.at > StaleFlight))
            fizzle.Add(Fizzled(stale, now));

        var sweep = all.Where(b => b.state is "exploded" or "fizzled" && now - b.at > Sweep).Select(b => b.bomb_id).ToHashSet();

        var drop = new WorldBomb
        {
            bomb_id = $"drop-{now / 1000}", state = "free",
            x = 1 + random.NextDouble() * (Width - 2), y = 1 + random.NextDouble() * (Depth - 2), z = DropHeight,
            dropped_at = now, at = now,
        };
        return (fizzle, sweep, drop, free.Count - oldest.Count + 1);
    }

    /// <summary>Each bomb as its row furthest through free, held, flying, exploded or fizzled says it is.</summary>
    internal static List<WorldBomb> Latest(IEnumerable<WorldBomb> rows) =>
        rows.GroupBy(b => b.bomb_id).Select(g => g.OrderByDescending(b => Rank(b.state)).ThenByDescending(b => b.at).First()).ToList();

    private static int Rank(string state) => state switch { "free" => 0, "held" => 1, "flying" => 2, _ => 3 };

    private static WorldBomb Fizzled(WorldBomb bomb, long now) => new()
    {
        bomb_id = bomb.bomb_id, state = "fizzled", holder = bomb.holder, x = bomb.x, y = bomb.y, z = bomb.z,
        dropped_at = bomb.dropped_at, at = now,
    };

    private static async Task<List<Record<WorldBomb>>> LoadAsync(ITable<WorldBomb> table, CancellationToken ct)
    {
        var rows = new List<Record<WorldBomb>>();
        string? cursor = null;
        do
        {
            var page = await table.Query().Take(200).WithCursor(cursor).ToPageAsync(ct);
            rows.AddRange(page.Items);
            cursor = page.NextCursor;
        } while (!string.IsNullOrEmpty(cursor));
        return rows;
    }
}
