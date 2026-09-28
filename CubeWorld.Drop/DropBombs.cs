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

public sealed record DropRequest(int? Drops);

/// <summary>
/// Drops a bomb on a parachute every 15 seconds. A cron fires once a minute at most, so each fire drops four, 15
/// seconds apart. At most five bombs lie free in the world: before a sixth comes, the oldest goes up in smoke.
/// </summary>
public sealed class DropBombs : PlatformFunction<DropRequest>
{
    public const int MaxFree = 5, DropsPerFire = 4, Width = 72, Depth = 24;
    public const double DropHeight = 32;
    public static readonly TimeSpan Interval = TimeSpan.FromSeconds(15);

    // A thrown bomb whose server went away never lands; it goes after half a minute. Finished bombs are swept
    // after two: every server has heard them by then.
    private const long StaleFlight = 30_000, Sweep = 120_000;

    protected override async Task<FunctionResponse> HandleAsync(DropRequest body, CancellationToken ct)
    {
        var drops = Math.Clamp(body.Drops ?? DropsPerFire, 1, DropsPerFire);
        var table = Platform.Table<WorldBomb>();
        var random = new Random();
        int dropped = 0, fizzled = 0, swept = 0;

        for (var i = 0; i < drops; i++)
        {
            if (i > 0) await Task.Delay(Interval, ct);
            var rows = await LoadAsync(table, ct);
            var now = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
            var plan = Plan(rows.Select(r => r.Fields!), now, random);

            foreach (var bomb in plan.Fizzle)
            {
                await table.UpsertByAsync(b => b.bomb_id, bomb.bomb_id, UpsertMode.Managed, bomb, ct);
                fizzled++;
            }
            foreach (var row in rows.Where(r => plan.Sweep.Contains(r.Fields!.bomb_id)))
            {
                try { await table.DeleteAsync(row.Id, ct); swept++; }
                catch (ApiException) { }
            }
            await table.UpsertByAsync(b => b.bomb_id, plan.Drop.bomb_id, UpsertMode.Managed, plan.Drop, ct);
            dropped++;
            await Platform.Log($"dropped bomb {plan.Drop.bomb_id} over ({plan.Drop.x:0.0}, {plan.Drop.y:0.0}), {plan.Free} free", ct: ct);
        }

        return FunctionResponse.Json(new { dropped, fizzled, swept });
    }

    /// <summary>What one drop does: which bombs go up in smoke, which finished ones are swept, and the new bomb.</summary>
    internal static (List<WorldBomb> Fizzle, HashSet<string> Sweep, WorldBomb Drop, int Free) Plan(IEnumerable<WorldBomb> bombs, long now, Random random)
    {
        var all = bombs.ToList();
        var fizzle = new List<WorldBomb>();

        var free = all.Where(b => b.state == "free").OrderBy(b => b.dropped_at).ToList();
        var oldest = free.Take(Math.Max(0, free.Count - (MaxFree - 1))).ToList();
        fizzle.AddRange(oldest.Select(b => Fizzled(b, now)));
        foreach (var stale in all.Where(b => b.state == "flying" && now - b.at > StaleFlight))
            fizzle.Add(Fizzled(stale, now));

        var sweep = all.Where(b => b.state is "exploded" or "fizzled" && now - b.at > Sweep).Select(b => b.bomb_id).ToHashSet();

        var drop = new WorldBomb
        {
            bomb_id = Guid.NewGuid().ToString("N"), state = "free",
            x = 1 + random.NextDouble() * (Width - 2), y = 1 + random.NextDouble() * (Depth - 2), z = DropHeight,
            dropped_at = now, at = now,
        };
        return (fizzle, sweep, drop, free.Count - oldest.Count + 1);
    }

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
