using CubeWorld.Server;
using Xunit;

namespace CubeWorld.Tests;

/// <summary>Rows the other writers put in the shared tables, read as they are in dev (PSV-2994).</summary>
public class SharedRowTests
{
    private static WorldCube Cube(int x, int y, int z, string kind, double? at) =>
        new() { key = $"{x}:{y}:{z}", x = x, y = y, z = z, kind = kind, placed_by = "p", placed_on = "8we6h", at = at };

    private static WorldCube OnServer(WorldCube cube, string server)
    {
        cube.placed_on = server;
        return cube;
    }

    [Fact]
    public void A_kind_an_Unreal_server_wrote_as_its_FName_prints_it_is_that_kind()
    {
        Assert.Equal("stone", Spec.Of("Stone").Kind);
        Assert.Equal("stone", Spec.Canonical("Stone"));
        Assert.Equal("diamond", Spec.Canonical("diamond"));
        Assert.Equal("air", Spec.Of("diamond").Kind);
    }

    [Fact]
    public void Stone_an_Unreal_server_placed_is_solid_here_can_be_dug_and_nothing_is_placed_into_it()
    {
        var world = new World();
        world.Load([Cube(31, 10, -1, "Stone", 1790769671508)]);
        world.Apply("upsert", Cube(42, 24, 0, "Stone", 1790766435296));

        Assert.Equal("stone", world.KindAt(31, 10, -1));
        Assert.True(world.IsSolid(42, 24, 0));
        Assert.Null(world.Place(42, 24, -1, 0, 0, 1, "dirt", "q", "sb70n", []));
        Assert.NotNull(world.Break(42, 24, 0, "q", "sb70n"));
        Assert.Equal("air", world.KindAt(42, 24, 0));
    }

    [Fact]
    public void Stone_and_stone_in_one_inventory_row_are_one_stack_of_the_larger_count()
    {
        var inventory = Inventory.Parse("""{"grass":64,"Stone":64,"stone":1}""");

        Assert.Equal(64, inventory.Count("stone"));
        Assert.Equal(64, inventory.Count("Stone"));
        Assert.DoesNotContain("Stone", inventory.Stacks.Keys);
        Assert.True(inventory.Take("Stone"));
        Assert.Equal(63, inventory.Stacks["stone"]);
    }

    [Fact]
    public void A_block_read_back_from_the_table_is_applied_when_this_server_missed_it()
    {
        var world = new World();
        world.Apply("upsert", Cube(4, 33, -2, "air", 1790896413000));

        Assert.True(world.Reconcile(Cube(4, 33, -2, "gold", 1790896476000), "2kk13", 1790896500000));
        Assert.Equal("gold", world.KindAt(4, 33, -2));
    }

    [Fact]
    public void A_block_read_back_that_is_older_than_what_this_server_holds_does_not_undo_it()
    {
        var world = new World();
        world.Apply("upsert", Cube(5, 5, 0, "brick", 2000));

        Assert.False(world.Reconcile(Cube(5, 5, 0, "air", 1000), "sb70n", 10_000));
        Assert.False(world.Reconcile(Cube(5, 5, 0, "air", null), "sb70n", 10_000));
        Assert.Equal("brick", world.KindAt(5, 5, 0));
    }

    [Fact]
    public void A_block_read_back_does_not_undo_this_servers_own_write_of_the_last_minute()
    {
        var world = new World();
        world.Apply("upsert", OnServer(Cube(6, 6, 0, "brick", 100_000), "sb70n"));

        // A row stamped later by a writer whose clock runs ahead, read before our own write landed.
        Assert.False(world.Reconcile(Cube(6, 6, 0, "air", 100_500), "sb70n", 130_000));
        Assert.Equal("brick", world.KindAt(6, 6, 0));
        Assert.True(world.Reconcile(Cube(6, 6, 0, "air", 100_500), "sb70n", 170_000));
    }

    [Fact]
    public void A_server_edits_for_players_in_its_region_or_just_past_its_border_only()
    {
        // Region 3, yellow: x 0..24, y 24..48.
        Assert.True(World.Near(3, 12, 36, Spec.BorderSlack));
        Assert.True(World.Near(3, 20.5, 22.5, Spec.BorderSlack));
        Assert.False(World.Near(3, 60.5, 5.5, Spec.BorderSlack));
        Assert.True(World.Near(3, 26.5, 36, Spec.BorderSlack));
        Assert.False(World.Near(3, 31.5, 36, Spec.BorderSlack));
        Assert.False(World.Near(-1, 12, 36, Spec.BorderSlack));
    }

    [Fact]
    public void A_player_past_the_border_whose_next_room_is_not_up_can_only_walk_there()
    {
        // Region 3, yellow: x 0..24, y 24..48; region 4, purple, beside it: x 24..48.
        int[] purpleUp = [4], nobodyElse = [];
        Assert.True(World.Serves(3, 12, 36, 0, nobodyElse));
        Assert.True(World.Serves(3, 12, 36, 60_000, nobodyElse));
        // Crossing into a live purple: the old server edits for the moment the crossing takes.
        Assert.True(World.Serves(3, 26.5, 36, 0, purpleUp));
        Assert.True(World.Serves(3, 26.5, 36, Spec.CrossingMs, purpleUp));
        // Purple's room did not let them in: they stay with yellow and can only walk.
        Assert.False(World.Serves(3, 26.5, 36, Spec.CrossingMs + 1, purpleUp));
        // No live server holds purple: not even for a moment.
        Assert.False(World.Serves(3, 26.5, 36, 0, nobodyElse));
        Assert.False(World.Serves(3, 31.5, 36, 0, purpleUp));
        Assert.False(World.Serves(-1, 12, 36, 0, purpleUp));

        // Blocks: yellow's own, or a live server's region; a region nobody holds keeps its blocks.
        Assert.True(World.ServesBlock(3, 23, 36, nobodyElse));
        Assert.True(World.ServesBlock(3, 24, 36, purpleUp));
        Assert.False(World.ServesBlock(3, 24, 36, nobodyElse));
        Assert.False(World.ServesBlock(-1, 23, 36, nobodyElse));
    }

    [Fact]
    public void A_player_another_server_hosts_shows_hurt_when_their_health_drops_between_two_fresh_poses()
    {
        var before = new WorldPresence { player_id = "v", health = 17, seen_at = 10_000 };

        Assert.True(WorldPresence.WasHurt(before, new WorldPresence { player_id = "v", health = 16, seen_at = 10_200 }));
        Assert.False(WorldPresence.WasHurt(before, new WorldPresence { player_id = "v", health = 18, seen_at = 10_200 }));
        Assert.False(WorldPresence.WasHurt(before, new WorldPresence { player_id = "v", health = 16, seen_at = 20_000 }));
        Assert.False(WorldPresence.WasHurt(before, new WorldPresence { player_id = "v", health = 0, seen_at = 10_200 }));
        Assert.False(WorldPresence.WasHurt(null, new WorldPresence { player_id = "v", health = 16, seen_at = 10_200 }));
    }

    [Fact]
    public void A_bomb_that_went_off_or_fizzled_is_remembered_as_over_for_fifteen_minutes()
    {
        var finished = new FinishedBombs();
        finished.Remember("drop-1", 1_000_000);

        Assert.True(finished.Has("drop-1", 1_000_000 + 60_000));
        Assert.False(finished.Has("drop-1", 1_000_000 + FinishedBombs.KeepMs));
        Assert.False(finished.Has("drop-2", 1_000_000));
    }

    [Fact]
    public void A_bomb_handed_out_again_long_after_its_drop_by_a_server_that_missed_its_end_is_a_ghost()
    {
        const long now = 1790898900000;
        var ghost = new WorldBomb { bomb_id = "drop-1790896989807", state = Bomb.Held, holder = "p", dropped_at = 1790896989807 - 600_000, at = now };
        var fresh = new WorldBomb { bomb_id = "drop-1790898800000", state = Bomb.Held, holder = "p", dropped_at = 1790898800000, at = now };

        Assert.True(Bomb.IsGhost(ghost, known: false, now));
        Assert.False(Bomb.IsGhost(ghost, known: true, now));
        Assert.False(Bomb.IsGhost(fresh, known: false, now));
        var over = new WorldBomb { bomb_id = ghost.bomb_id, state = Bomb.Fizzled, dropped_at = ghost.dropped_at, at = now };
        Assert.False(Bomb.IsGhost(over, known: false, now));
    }
}
