using PlayServ.Sdk;
using PlayServ.Sdk.Data;
using BombRows = System.Linq.IGrouping<string, PlayServ.Sdk.Data.Record<CubeWorld.Drop.WorldBomb>>;

namespace CubeWorld.Drop;

[EntityName("WorldBomb")]
public sealed class WorldBomb
{
    public string bomb_id { get; set; } = "";
    public string state { get; set; } = "";
    public double x { get; set; }
    public double y { get; set; }
    public double z { get; set; }
    public long dropped_at { get; set; }
    public long at { get; set; }
}

/// Every minute: 4 bombs, one every 15 seconds. At most 5 lie free.
public sealed class DropBombs : PlatformFunction<object>
{
    private const int BombsPerRun = 4, MaxFreeBombs = 5;

    private ITable<WorldBomb> WorldBombs => Platform.Table<WorldBomb>();

    protected override async Task<FunctionResponse> HandleAsync(object _, CancellationToken ct)
    {
        for (var i = 0; i < BombsPerRun; i++)
        {
            if (i > 0)
            {
                await Task.Delay(TimeSpan.FromSeconds(15), ct);
            }

            var now = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
            var bombs = await LoadBombs(ct);

            await ForgetFinishedBombs(bombs, now, ct);
            await FizzleOldestFreeBombs(bombs, now, ct);
            await DropNewBomb(now, ct);
        }

        return FunctionResponse.Json(new { ok = true });
    }

    /// Every bomb with all its rows: this function writes the first one, the servers add theirs as it is picked up and thrown.
    private async Task<List<BombRows>> LoadBombs(CancellationToken ct)
    {
        var rows = await WorldBombs.Query().Take(200).ToListAsync(ct);
        return rows.GroupBy(r => r.Fields!.bomb_id).ToList();
    }

    /// A bomb that went off over two minutes ago: forget it.
    private async Task ForgetFinishedBombs(List<BombRows> bombs, long now, CancellationToken ct)
    {
        foreach (var bomb in bombs.Where(b => WentOffLongAgo(b, now)))
        {
            foreach (var row in bomb)
            {
                await WorldBombs.DeleteAsync(row.Id, ct);
            }
        }
    }

    /// Five bombs already free: the oldest goes up in smoke, to make room for the new one.
    private async Task FizzleOldestFreeBombs(List<BombRows> bombs, long now, CancellationToken ct)
    {
        var free = bombs
            .Where(IsFree)
            .Select(b => b.First().Fields!)
            .OrderBy(b => b.dropped_at)
            .ToList();

        foreach (var old in free.Take(free.Count - (MaxFreeBombs - 1)))
        {
            await WorldBombs.CreateAsync(Bomb(old.bomb_id, "fizzled", old.x, old.y, old.dropped_at, now), ct);
        }
    }

    /// A new bomb somewhere over the world.
    private async Task DropNewBomb(long now, CancellationToken ct)
    {
        await WorldBombs.CreateAsync(Bomb($"drop-{now}", "free", Random.Shared.Next(1, 71), Random.Shared.Next(1, 23), now, now), ct);
    }

    /// No server has picked the bomb up yet.
    private static bool IsFree(BombRows bomb) => bomb.All(r => r.Fields!.state == "free");

    private static bool WentOffLongAgo(BombRows bomb, long now) =>
        bomb.Any(r => r.Fields!.state is "exploded" or "fizzled" && now - r.Fields!.at > 120_000);

    private static WorldBomb Bomb(string id, string state, double x, double y, long droppedAt, long now) =>
        new() { bomb_id = id, state = state, x = x, y = y, z = 32, dropped_at = droppedAt, at = now };
}
