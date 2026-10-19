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

/// Called by URL (POST /fn/cubeworld-reset): the whole world goes back to the generated terrain. One call clears the
/// WorldCube table (up to 100 000 rows, all or none). Every server hears it as one delete_where and takes its changed
/// blocks back to the terrain, showing the players a hundred a tick, so the world still goes block by block.
public sealed class ResetWorld : PlatformFunction<object>
{
    protected override async Task<FunctionResponse> HandleAsync(object _, CancellationToken ct) =>
        FunctionResponse.Json(new { ok = true, deleted = await Platform.Table<WorldCube>().ClearAsync(ct) });
}
