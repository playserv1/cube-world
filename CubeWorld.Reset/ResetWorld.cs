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

/// Called by URL (POST /fn/cubeworld-reset): the whole world goes back to the generated terrain. Every changed
/// block is deleted, the three regions at once and 16 blocks at a time in each, and every server and client hears
/// the deletes, so all three regions come back block by block together while everyone watches.
public sealed class ResetWorld : PlatformFunction<object>
{
    private const int RegionSize = 24, AtOnce = 16;

    private ITable<WorldCube> WorldCubes => Platform.Table<WorldCube>();

    protected override async Task<FunctionResponse> HandleAsync(object _, CancellationToken ct)
    {
        var regions = (await LoadChangedBlocks(ct)).GroupBy(row => RegionOf(row.Fields!.x)).ToList();
        await Task.WhenAll(regions.Select(region => DeleteAll(region, ct)));

        return FunctionResponse.Json(new
        {
            ok = true,
            deleted = regions.Sum(r => r.Count()),
            regions = regions.OrderBy(r => r.Key).ToDictionary(r => r.Key.ToString(), r => r.Count()),
        });
    }

    /// The region a column belongs to, as the servers count them: region r is the columns r·24 up to the next region's first.
    private static int RegionOf(int x) => (int)Math.Floor(x / (double)RegionSize);

    private Task DeleteAll(IEnumerable<Record<WorldCube>> rows, CancellationToken ct) =>
        Parallel.ForEachAsync(rows, new ParallelOptions { MaxDegreeOfParallelism = AtOnce, CancellationToken = ct },
            async (row, token) => await WorldCubes.DeleteAsync(row.Id, token));

    /// Every row of WorldCube, 200 a page.
    private async Task<List<Record<WorldCube>>> LoadChangedBlocks(CancellationToken ct)
    {
        var rows = new List<Record<WorldCube>>();
        string? cursor = null;
        do
        {
            var page = await WorldCubes.Query().Take(200).WithCursor(cursor).ToPageAsync(ct);
            rows.AddRange(page.Items);
            cursor = page.NextCursor;
        } while (!string.IsNullOrEmpty(cursor));
        return rows;
    }
}
