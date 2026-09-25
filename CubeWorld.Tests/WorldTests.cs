using CubeWorld.Server;
using Xunit;

namespace CubeWorld.Tests;

public class WorldTests
{
    [Fact]
    public void Cubes_stack_in_a_column()
    {
        var world = new World();

        var first = world.Place(3, 4, "stone", "p1", "a");
        var second = world.Place(3, 4, "wood", "p1", "a");

        Assert.Equal("3:4:0", first!.key);
        Assert.Equal("3:4:1", second!.key);
    }

    [Fact]
    public void A_full_column_takes_no_more_cubes()
    {
        var world = new World();
        for (var i = 0; i < World.Height; i++) Assert.NotNull(world.Place(0, 0, "stone", "p", "a"));

        Assert.Null(world.Place(0, 0, "stone", "p", "a"));
    }

    [Theory]
    [InlineData(-1, 0, "stone")]
    [InlineData(0, World.Depth, "stone")]
    [InlineData(World.Width, 0, "stone")]
    [InlineData(1, 1, "lava")]
    public void Outside_the_world_or_an_unknown_kind_is_refused(int x, int y, string kind) =>
        Assert.Null(new World().Place(x, y, kind, "p", "a"));

    [Fact]
    public void Break_takes_the_top_cube()
    {
        var world = new World();
        world.Place(2, 2, "stone", "p", "a");
        world.Place(2, 2, "gold", "p", "a");

        Assert.Equal("gold", world.Break(2, 2)!.kind);
        Assert.Equal("stone", Assert.Single(world.Cubes).kind);
    }

    [Fact]
    public void Break_on_empty_ground_finds_nothing() =>
        Assert.Null(new World().Break(5, 5));

    [Fact]
    public void A_cube_from_another_server_is_applied_once()
    {
        var world = new World();
        var cube = new WorldCube { key = "1:2:0", x = 1, y = 2, z = 0, kind = "brick", placed_on = "other" };

        Assert.True(world.Apply("upsert", cube));
        Assert.False(world.Apply("upsert", cube));
        Assert.True(world.Apply("delete", cube));
        Assert.Empty(world.Cubes);
    }

    [Fact]
    public void A_cube_from_another_server_outside_the_world_is_ignored() =>
        Assert.False(new World().Apply("upsert", new WorldCube { key = "99:0:0", x = 99, kind = "stone" }));

    [Fact]
    public void A_placed_cube_takes_the_lowest_free_level_after_a_remote_break()
    {
        var world = new World();
        world.Load([
            new WorldCube { key = "0:0:0", x = 0, y = 0, z = 0, kind = "stone" },
            new WorldCube { key = "0:0:1", x = 0, y = 0, z = 1, kind = "stone" },
        ]);
        world.Apply("delete", new WorldCube { key = "0:0:0" });

        Assert.Equal("0:0:0", world.Place(0, 0, "wood", "p", "a")!.key);
    }
}
