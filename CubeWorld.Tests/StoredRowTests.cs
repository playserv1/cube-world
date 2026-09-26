using System.Text.Json;
using CubeWorld.Server;
using Xunit;

namespace CubeWorld.Tests;

/// <summary>
/// Rows as the platform hands them back. A field added to a table after rows were written comes back as null in the
/// old rows, and a C# server that cannot read one of them never loads the world and never opens its room.
/// </summary>
public class StoredRowTests
{
    // The options PlayServ.Sdk reads a record's fields with (Platform.Data.cs, DataJsonOptions).
    private static readonly JsonSerializerOptions Sdk = new(JsonSerializerDefaults.Web) { PropertyNamingPolicy = null };

    [Fact]
    public void A_block_written_before_blocks_carried_a_time_still_loads()
    {
        // A dev row from 2026-09-30, written by a C# server before WorldCube.at existed.
        const string row = """
            {"id":"rec_7T91C1XK0BEMS9X1BTY94DQX0F","key":"55:9:63","x":55,"y":9,"z":63,"kind":"glass",
             "placed_by":"plr_0Y6CEASG83WKXM1YYQBE2WFM","placed_on":"sbj8q","at":null}
            """;

        var cube = JsonSerializer.Deserialize<WorldCube>(row, Sdk)!;

        Assert.Equal(("55:9:63", "glass"), (cube.key, cube.kind));
        Assert.Null(cube.at);
    }

    [Fact]
    public void A_block_time_written_by_an_Unreal_server_as_a_double_loads()
    {
        var cube = JsonSerializer.Deserialize<WorldCube>("""{"key":"31:30:0","kind":"grass","at":1790769792521.0}""", Sdk)!;

        Assert.Equal(1790769792521d, cube.at);
    }

    [Fact]
    public void A_hit_without_a_time_loads()
    {
        var hit = JsonSerializer.Deserialize<WorldHit>("""{"hit_id":"h","victim":"v","at":null}""", Sdk)!;

        Assert.Null(hit.at);
    }
}
