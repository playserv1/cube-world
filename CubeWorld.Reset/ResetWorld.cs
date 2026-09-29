using System.Net.Http.Headers;
using System.Net.Http.Json;
using System.Text.Json;
using PlayServ.Sdk;
using PlayServ.Sdk.Data;

namespace CubeWorld.Reset;

[EntityName("WorldCube")]
public sealed class WorldCube
{
    public string key { get; set; } = "";
    public int x { get; set; }
    public int y { get; set; }
    public int z { get; set; }
    public string kind { get; set; } = "";
    public string placed_by { get; set; } = "";
    public string placed_on { get; set; } = "";
}

[EntityName("WorldEpoch")]
public sealed class WorldEpoch
{
    public string name { get; set; } = "world";
    public long epoch { get; set; }
    public long at { get; set; }
    public string by { get; set; } = "";
    public int seeded { get; set; }
}

public sealed record ResetRequest(string? By);

/// <summary>
/// The whole world back to its default state, in three platform calls rather than one write per block:
/// every changed block is cleared in one call, the default blocks are inserted in batches of 200 with the
/// platform's bulk insert, and the epoch is bumped so every game server restarts and opens its room fresh
/// from the table. Called by a game server when an operator closes a room or a player asks for a reset, or
/// by an operator with a server key.
/// </summary>
public sealed class ResetWorld : PlatformFunction<ResetRequest>
{
    /// <summary>The platform caps a bulk insert at 200 rows per call.</summary>
    public const int BatchSize = 200;

    protected override async Task<FunctionResponse> HandleAsync(ResetRequest body, CancellationToken ct)
    {
        var by = body?.By ?? "operator";
        var cleared = await Platform.Records.Clear("WorldCube", ct);
        var seed = Seed.Blocks();
        var batches = await BulkInsert(seed, ct);
        var epoch = await BumpEpoch(by, seed.Count, ct);

        await Platform.Log($"world reset by {by}: {cleared} changed blocks cleared, {seed.Count} default blocks inserted in {batches} batches, epoch {epoch}", ct: ct);
        return FunctionResponse.Json(new { cleared, inserted = seed.Count, batches, epoch });
    }

    /// <summary>POST /data/tables/WorldCube/records:bulk-create, 200 rows a call, the calls in parallel.</summary>
    private static async Task<int> BulkInsert(IReadOnlyList<WorldCube> rows, CancellationToken ct)
    {
        var api = Environment.GetEnvironmentVariable("PLAYSERV_API_URL") ?? Environment.GetEnvironmentVariable("CUBEWORLD_API_URL")
                  ?? throw new InvalidOperationException("CUBEWORLD_API_URL is not set");
        var key = Environment.GetEnvironmentVariable("CUBEWORLD_SERVER_KEY")
                  ?? throw new InvalidOperationException("the CUBEWORLD_SERVER_KEY secret is not set");
        using var http = new HttpClient { BaseAddress = new Uri(api.TrimEnd('/') + "/") };
        http.DefaultRequestHeaders.Authorization = new AuthenticationHeaderValue("Bearer", key);

        var batches = rows.Chunk(BatchSize).Select(async batch =>
        {
            var response = await http.PostAsJsonAsync("data/tables/WorldCube/records:bulk-create", new { records = batch }, ct);
            if (!response.IsSuccessStatusCode)
                throw new InvalidOperationException($"bulk insert refused: {(int)response.StatusCode} {await response.Content.ReadAsStringAsync(ct)}");
            var answer = await response.Content.ReadFromJsonAsync<JsonElement>(ct);
            return answer.GetProperty("created").GetInt32();
        }).ToArray();

        var created = await Task.WhenAll(batches);
        if (created.Sum() != rows.Count) throw new InvalidOperationException($"bulk insert wrote {created.Sum()} of {rows.Count} rows");
        return batches.Length;
    }

    private static async Task<long> BumpEpoch(string by, int seeded, CancellationToken ct)
    {
        var epochs = Platform.Table<WorldEpoch>();
        var current = (await epochs.FindByAsync(e => e.name, "world", ct))?.Fields?.epoch ?? 0;
        var next = new WorldEpoch { name = "world", epoch = current + 1, at = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds(), by = by, seeded = seeded };
        await epochs.UpsertByAsync(e => e.name, "world", UpsertMode.Managed, next, ct);
        return next.epoch;
    }
}

/// <summary>
/// The default content of the world, on top of the superflat terrain the servers generate: twelve oaks, four
/// per region, clear of the spawn at each region's centre. An oak is a trunk of five logs, two 5 × 5 layers
/// of leaves without their corners around the top two logs, a 3 × 3 layer above the trunk and a cross on top.
/// </summary>
public static class Seed
{
    public const int RegionSize = 24, Regions = 3;

    public static readonly (int x, int y)[] Trees =
        Enumerable.Range(0, Regions).SelectMany(r => new[] { (r * RegionSize + 4, 5), (r * RegionSize + 18, 4), (r * RegionSize + 6, 18), (r * RegionSize + 19, 17) }).ToArray();

    public static List<WorldCube> Blocks()
    {
        var blocks = new Dictionary<string, WorldCube>();
        void Put(int x, int y, int z, string kind, bool overwrite)
        {
            var key = $"{x}:{y}:{z}";
            if (!overwrite && blocks.ContainsKey(key)) return;
            blocks[key] = new WorldCube { key = key, x = x, y = y, z = z, kind = kind, placed_by = "seed", placed_on = "seed" };
        }
        foreach (var (tx, ty) in Trees)
        {
            for (var dz = 0; dz < 5; dz++) Put(tx, ty, dz, "wood", overwrite: true);
            for (var dx = -2; dx <= 2; dx++)
                for (var dy = -2; dy <= 2; dy++)
                {
                    var corner = Math.Abs(dx) == 2 && Math.Abs(dy) == 2;
                    var trunk = dx == 0 && dy == 0;
                    for (var dz = 3; dz <= 4; dz++) if (!corner && !trunk) Put(tx + dx, ty + dy, dz, "leaves", overwrite: false);
                    if (Math.Abs(dx) <= 1 && Math.Abs(dy) <= 1) Put(tx + dx, ty + dy, 5, "leaves", overwrite: false);
                    if (Math.Abs(dx) + Math.Abs(dy) <= 1) Put(tx + dx, ty + dy, 6, "leaves", overwrite: false);
                }
        }
        return blocks.Values.ToList();
    }
}
