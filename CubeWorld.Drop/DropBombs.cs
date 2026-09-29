using PlayServ.Sdk;
using PlayServ.Sdk.Data;

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
    protected override async Task<FunctionResponse> HandleAsync(object _, CancellationToken ct)
    {
        var table = Platform.Table<WorldBomb>();

        for (var i = 0; i < 4; i++)
        {
            if (i > 0) await Task.Delay(TimeSpan.FromSeconds(15), ct);
            var now = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds();
            var bombs = (await table.Query().Take(200).ToListAsync(ct)).GroupBy(r => r.Fields!.bomb_id).ToList();

            // 1. A bomb that went off over two minutes ago: forget it.
            foreach (var bomb in bombs.Where(b => b.Any(r => r.Fields!.state is "exploded" or "fizzled" && now - r.Fields!.at > 120_000)))
                foreach (var row in bomb) await table.DeleteAsync(row.Id, ct);

            // 2. Five bombs already free (no server has touched them): the oldest goes up in smoke.
            var free = bombs.Where(b => b.All(r => r.Fields!.state == "free")).Select(b => b.First().Fields!)
                            .OrderBy(b => b.dropped_at).ToList();
            foreach (var old in free.Take(free.Count - 4))
                await table.CreateAsync(Bomb(old.bomb_id, "fizzled", old.x, old.y, old.dropped_at, now), ct);

            // 3. A new bomb somewhere over the world.
            await table.CreateAsync(Bomb($"drop-{now}", "free", Random.Shared.Next(1, 71), Random.Shared.Next(1, 23), now, now), ct);
        }

        return FunctionResponse.Json(new { ok = true });
    }

    static WorldBomb Bomb(string id, string state, double x, double y, long droppedAt, long now) =>
        new() { bomb_id = id, state = state, x = x, y = y, z = 32, dropped_at = droppedAt, at = now };
}
