using CubeWorld.Server;
using Xunit;

namespace CubeWorld.Tests;

public class WorldTests
{
    private static readonly Hitbox[] Nobody = [];

    [Fact]
    public void The_ground_is_superflat_bedrock_dirt_dirt_grass()
    {
        var world = new World();

        Assert.Equal("bedrock", world.KindAt(5, 5, -4));
        Assert.Equal("dirt", world.KindAt(5, 5, -3));
        Assert.Equal("dirt", world.KindAt(5, 5, -2));
        Assert.Equal("grass", world.KindAt(5, 5, -1));
        Assert.Equal("air", world.KindAt(5, 5, 0));
    }

    [Fact]
    public void An_oak_stands_at_each_tree_spot_and_its_logs_can_be_cut()
    {
        var world = new World();
        var (x, y) = Spec.Trees[0];

        Assert.Equal("wood", world.KindAt(x, y, 0));
        Assert.Equal("wood", world.KindAt(x, y, 4));
        Assert.Equal("leaves", world.KindAt(x, y, 5));
        Assert.Equal("leaves", world.KindAt(x + 2, y, 3));
        Assert.Equal("air", world.KindAt(x + 2, y + 2, 3));
        Assert.Equal("air", world.KindAt(x, y, 7));
        Assert.Equal(12, Spec.Trees.Length);

        var (update, log) = world.Break(x, y, 0, "p", "a")!.Value;
        Assert.Equal("wood", log.Kind);
        Assert.Equal(("upsert", "air"), (update.Changes[0].Op, update.Changes[0].Cube.kind));
        Assert.Equal("air", world.KindAt(x, y, 0));
    }

    [Fact]
    public void A_block_is_placed_against_the_face_that_was_clicked()
    {
        var world = new World();

        var onTop = world.Place(3, 4, -1, 0, 0, 1, "stone", "p1", "a", Nobody);
        var beside = world.Place(3, 4, 0, 1, 0, 0, "wood", "p1", "a", Nobody);

        Assert.Equal("3:4:0", Assert.Single(onTop!.Changes).Cube.key);
        Assert.Equal("4:4:0", Assert.Single(beside!.Changes).Cube.key);
        Assert.Equal("wood", world.KindAt(4, 4, 0));
    }

    [Fact]
    public void A_block_needs_a_face_to_be_placed_against() =>
        Assert.Null(new World().Place(3, 4, 2, 0, 0, 1, "stone", "p", "a", Nobody));

    [Fact]
    public void A_block_cannot_go_where_a_block_already_is()
    {
        var world = new World();
        world.Place(3, 4, -1, 0, 0, 1, "stone", "p", "a", Nobody);

        Assert.Null(world.Place(3, 4, -1, 0, 0, 1, "stone", "p", "a", Nobody));
        Assert.Null(world.Place(3, 4, 0, 0, 0, -1, "stone", "p", "a", Nobody));
    }

    [Fact]
    public void A_block_cannot_be_placed_inside_a_player()
    {
        var world = new World();
        var standing = new Hitbox(3.5, 4.5, 0, Spec.PlayerHeight);

        Assert.Null(world.Place(3, 4, -1, 0, 0, 1, "stone", "p", "a", [standing]));
        Assert.NotNull(world.Place(3, 4, -1, 0, 0, 1, "stone", "p", "a", [standing with { X = 4.4 }]));
    }

    [Theory]
    [InlineData(-1, 0, "stone")]
    [InlineData(0, World.Depth, "stone")]
    [InlineData(World.Width, 0, "stone")]
    [InlineData(1, 1, "lava")]
    [InlineData(1, 1, "bedrock")]
    [InlineData(1, 1, "air")]
    public void Outside_the_world_or_an_unplaceable_kind_is_refused(int x, int y, string kind) =>
        Assert.Null(new World().Place(x, y, -1, 0, 0, 1, kind, "p", "a", Nobody));

    [Fact]
    public void The_world_has_a_ceiling()
    {
        var world = new World();
        for (var z = 0; z < World.MaxZ; z++) Assert.NotNull(world.Place(0, 0, z - 1, 0, 0, 1, "stone", "p", "a", Nobody));

        Assert.Null(world.Place(0, 0, World.MaxZ - 1, 0, 0, 1, "stone", "p", "a", Nobody));
    }

    [Fact]
    public void Breaking_any_block_writes_air_so_every_server_hears_it()
    {
        var world = new World();
        world.Place(2, 2, -1, 0, 0, 1, "gold", "p", "a", Nobody);

        var (placed, gold) = world.Break(2, 2, 0, "p", "a")!.Value;
        var (generated, grass) = world.Break(2, 2, -1, "p", "a")!.Value;

        Assert.Equal("gold", gold.Kind);
        Assert.Equal(("upsert", "air"), (Assert.Single(placed.Changes).Op, placed.Changes[0].Cube.kind));
        Assert.Equal("grass", grass.Kind);
        Assert.Equal(("upsert", "air"), (Assert.Single(generated.Changes).Op, generated.Changes[0].Cube.kind));
        Assert.Equal("air", world.KindAt(2, 2, -1));
        Assert.Equal("air", world.KindAt(2, 2, 0));
        Assert.Equal(2, world.Overrides.Count());
    }

    [Fact]
    public void Bedrock_and_air_cannot_be_broken()
    {
        var world = new World();

        Assert.Null(world.Break(5, 5, -4, "p", "a"));
        Assert.Null(world.Break(5, 5, 0, "p", "a"));
    }

    [Theory]
    [InlineData("grass", 18)]   // hardness 0.6, no tool needed: 0.9 s
    [InlineData("dirt", 15)]    // 0.5: 0.75 s
    [InlineData("sand", 15)]
    [InlineData("glass", 9)]    // 0.3: 0.45 s
    [InlineData("wood", 60)]    // 2.0: 3 s
    [InlineData("stone", 150)]  // 1.5, needs a pickaxe: 7.5 s by hand
    [InlineData("brick", 200)]  // 2.0, needs a pickaxe: 10 s
    [InlineData("gold", 300)]   // 3.0, needs a pickaxe: 15 s
    public void Breaking_by_hand_takes_the_wiki_time(string kind, int ticks) =>
        Assert.Equal(ticks, Spec.Of(kind).BreakTicks);

    [Fact]
    public void Grass_drops_dirt_and_stone_drops_nothing_by_hand()
    {
        Assert.Equal("dirt", Spec.Of("grass").Drop);
        Assert.Null(Spec.Of("stone").Drop);
        Assert.Equal("sand", Spec.Of("sand").Drop);
    }

    [Fact]
    public void Sand_falls_when_the_block_under_it_is_broken()
    {
        var world = new World();
        world.Place(1, 1, -1, 0, 0, 1, "stone", "p", "a", Nobody);
        world.Place(1, 1, 0, 0, 0, 1, "sand", "p", "a", Nobody);
        world.Place(1, 1, 1, 0, 0, 1, "sand", "p", "a", Nobody);

        var (update, _) = world.Break(1, 1, 0, "p", "a")!.Value;

        Assert.Equal([new Fall("sand", 1, 1, 1, 0), new Fall("sand", 1, 1, 2, 1)], update.Falls);
        Assert.Equal("sand", world.KindAt(1, 1, 0));
        Assert.Equal("sand", world.KindAt(1, 1, 1));
        Assert.Equal("air", world.KindAt(1, 1, 2));
    }

    [Fact]
    public void Sand_placed_against_a_wall_falls_to_the_ground()
    {
        var world = new World();
        world.Place(1, 1, -1, 0, 0, 1, "stone", "p", "a", Nobody);
        world.Place(1, 1, 0, 0, 0, 1, "stone", "p", "a", Nobody);

        var update = world.Place(1, 1, 1, 1, 0, 0, "sand", "p", "a", Nobody)!;

        Assert.Equal(new Fall("sand", 2, 1, 1, 0), Assert.Single(update.Falls));
        Assert.Equal("sand", world.KindAt(2, 1, 0));
        Assert.Equal("air", world.KindAt(2, 1, 1));
    }

    [Fact]
    public void Stone_does_not_fall()
    {
        var world = new World();
        world.Place(1, 1, -1, 0, 0, 1, "stone", "p", "a", Nobody);
        world.Place(1, 1, 0, 0, 0, 1, "stone", "p", "a", Nobody);

        Assert.Empty(world.Break(1, 1, 0, "p", "a")!.Value.Update.Falls);
    }

    [Fact]
    public void A_cube_from_another_server_is_applied_once()
    {
        var world = new World();
        var cube = new WorldCube { key = "1:2:0", x = 1, y = 2, z = 0, kind = "brick", placed_on = "other" };

        Assert.True(world.Apply("upsert", cube));
        Assert.False(world.Apply("upsert", cube));
        Assert.True(world.Apply("delete", cube));
        Assert.Empty(world.Overrides);
    }

    [Fact]
    public void Air_from_another_server_digs_out_the_ground()
    {
        var world = new World();

        Assert.True(world.Apply("upsert", new WorldCube { key = "1:2:-1", x = 1, y = 2, z = -1, kind = "air" }));
        Assert.False(world.IsSolid(1, 2, -1));
    }

    [Fact]
    public void A_cube_from_another_server_outside_the_world_is_ignored() =>
        Assert.False(new World().Apply("upsert", new WorldCube { key = "99:0:0", x = 99, kind = "stone" }));

    [Fact]
    public void Reach_is_measured_to_the_nearest_point_of_the_block()
    {
        Assert.Equal(0, World.DistanceToBlock(0.5, 0.5, 0.5, 0, 0, 0));
        Assert.Equal(4.5, World.DistanceToBlock(0.5, 0.5, 0.5, 0, 0, 5), 9);
        Assert.Equal(Math.Sqrt(2), World.DistanceToBlock(-1, -1, 0.5, 0, 0, 0), 9);
    }

    [Fact]
    public void Inventory_starts_full_gives_back_drops_and_stops_at_a_stack()
    {
        var inventory = Inventory.Starting();

        Assert.Equal(Spec.StartingStack, inventory.Count("stone"));
        Assert.False(inventory.Give("stone"));
        Assert.True(inventory.Take("stone"));
        Assert.True(inventory.Give("stone"));
        Assert.False(inventory.Give("bedrock"));
        Assert.Equal(inventory.Stacks, Inventory.Parse(inventory.ToRecord("p").stacks).Stacks);
    }
}
