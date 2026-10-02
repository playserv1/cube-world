using CubeWorld.Drop;
using CubeWorld.Refill;
using Xunit;

namespace CubeWorld.Tests;

/// <summary>The refill and drop functions' rules on the rows that are in dev (PSV-2994).</summary>
public class FunctionTests
{
    [Fact]
    public void A_refill_folds_an_Unreal_servers_Stone_into_stone_and_tops_every_kind_up_by_one()
    {
        var after = RefillInventories.Refilled(new Dictionary<string, int> { ["Stone"] = 64, ["stone"] = 2, ["dirt"] = 61 });

        Assert.DoesNotContain("Stone", after.Keys);
        Assert.Equal(64, after["stone"]);
        Assert.Equal(62, after["dirt"]);
        Assert.Equal(1, after["gold"]);
    }

    [Fact]
    public void A_full_inventory_needs_no_write()
    {
        var full = new[] { "grass", "dirt", "sand", "stone", "wood", "brick", "glass", "gold", "leaves" }.ToDictionary(k => k, _ => 64);

        Assert.True(RefillInventories.Same(full, RefillInventories.Refilled(full)));
        Assert.False(RefillInventories.Same(new Dictionary<string, int>(full) { ["Stone"] = 64 }, RefillInventories.Refilled(full)));
    }

    private static Drop.WorldBomb Row(string state, long at, string? holder = null) =>
        new() { bomb_id = "drop-1", state = state, holder = holder, dropped_at = 1000, at = at };

    [Fact]
    public void A_bomb_is_its_row_furthest_on()
    {
        Assert.Equal("held", DropBombs.Furthest([Row("free", 1000), Row("held", 2000, "p")]).state);
        Assert.Equal("fizzled", DropBombs.Furthest([Row("held", 3000, "p"), Row("fizzled", 2000), Row("free", 1000)]).state);
    }

    [Fact]
    public void A_holder_is_gone_when_no_server_has_written_their_presence_for_a_minute()
    {
        const long now = 10_000_000;

        Assert.True(DropBombs.HolderGone(null, now));
        Assert.True(DropBombs.HolderGone(now - DropBombs.HolderGoneMs - 1, now));
        Assert.False(DropBombs.HolderGone(now - 2_000, now));
    }

    [Fact]
    public void A_bomb_that_went_off_over_two_minutes_ago_is_forgotten()
    {
        Assert.True(DropBombs.WentOffLongAgo([Row("free", 0), Row("exploded", 1000)], 1000 + 120_001));
        Assert.False(DropBombs.WentOffLongAgo([Row("free", 0), Row("exploded", 1000)], 1000 + 60_000));
    }

    [Fact]
    public void Bombs_fall_over_every_region_a_server_holds_now_and_nowhere_else()
    {
        const long now = 10_000_000;
        var rows = new[]
        {
            new Drop.WorldRegion { region = "0", seen_at = now - 5_000 },
            new Drop.WorldRegion { region = "4", seen_at = now - 1_000 },
            new Drop.WorldRegion { region = "2", seen_at = now - DropBombs.RegionGoneMs - 1 },
            new Drop.WorldRegion { region = "x", seen_at = now },
        };

        Assert.Equal([0, 4], DropBombs.LiveRegions(rows, now));
    }

    [Theory]
    [InlineData(0)]
    [InlineData(2)]
    [InlineData(4)]
    public void A_regions_two_bombs_come_down_on_different_blocks_inside_it(int region)
    {
        for (var seed = 0; seed < 200; seed++)
        {
            var spots = DropBombs.Spots(region, new Random(seed));

            Assert.Equal(DropBombs.BombsPerRegion, spots.Count);
            Assert.Equal(spots.Count, spots.Distinct().Count());
            Assert.All(spots, s => Assert.Equal(region, DropBombs.RegionOf(s.X, s.Y)));
        }
    }

    [Fact]
    public void A_region_keeps_at_most_five_free_bombs_and_the_others_regions_are_not_counted()
    {
        var bombs = Enumerable.Range(0, 5)
            .Select(i => new Drop.WorldBomb { bomb_id = $"a{i}", state = "free", x = 5, y = 5, dropped_at = 1000 + i })
            .Append(new Drop.WorldBomb { bomb_id = "held", state = "held", x = 6, y = 6, dropped_at = 500 })
            .Append(new Drop.WorldBomb { bomb_id = "other", state = "free", x = 30, y = 5, dropped_at = 100 })
            .ToList();

        var fizzled = DropBombs.OldestFreeToFizzle(bombs, region: 0).Select(b => b.bomb_id).ToList();

        Assert.Equal(["a0", "a1"], fizzled);
        Assert.Empty(DropBombs.OldestFreeToFizzle(bombs, region: 1));
    }
}
