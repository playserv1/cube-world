using CubeWorld.Drop;
using CubeWorld.Server;
using Xunit;

namespace CubeWorld.Tests;

public class BombTests
{
    private static readonly (string, Hitbox)[] Nobody = [];

    [Fact]
    public void An_explosion_digs_a_crater_but_never_through_bedrock()
    {
        var world = new World();

        var update = world.Explode(30.5, 12.5, 0, Spec.BombPower, new Random(1), "p", "a");

        Assert.Equal("air", world.KindAt(30, 12, -1));
        Assert.Equal("bedrock", world.KindAt(30, 12, -4));
        Assert.All(update.Changes, c => Assert.Equal("air", c.Cube.kind));
        Assert.InRange(update.Changes.Count, 20, 200);
        Assert.Equal("grass", world.KindAt(36, 12, -1));
    }

    [Fact]
    public void Stone_stands_up_to_a_blast_that_takes_dirt()
    {
        var world = new World();
        for (var x = 27; x <= 29; x++) world.Place(x, 12, -1, 0, 0, 1, "stone", "p", "a", []);
        world.Place(33, 12, -1, 0, 0, 1, "dirt", "p", "a", []);

        world.Explode(31, 12.5, 0.5, Spec.BombPower, new Random(1), "p", "a");

        Assert.Equal("stone", world.KindAt(28, 12, 0));
        Assert.Equal("air", world.KindAt(33, 12, 0));
    }

    [Fact]
    public void Sand_over_a_crater_falls_into_it()
    {
        var world = new World();
        world.Place(30, 12, -1, 0, 0, 1, "stone", "p", "a", []);
        for (var z = 0; z < 6; z++) world.Place(30, 12, z, 0, 0, 1, "sand", "p", "a", []);

        var update = world.Explode(30.5, 12.5, 0.5, Spec.BombPower, new Random(1), "p", "a");

        Assert.NotEmpty(update.Falls);
        Assert.True(world.IsSolid(30, 12, -4));
    }

    [Fact]
    public void A_blast_hurts_most_up_close_and_not_at_all_beyond_twice_its_power()
    {
        var world = new World();
        Hitbox At(double x) => new(x, 12.5, 0, Spec.PlayerHeight);

        var close = world.Blast(30, 12.5, 0.5, Spec.BombPower, At(30.5), Spec.EyeHeight)!.Value;
        var far = world.Blast(30, 12.5, 0.5, Spec.BombPower, At(34), Spec.EyeHeight)!.Value;

        Assert.True(close.Damage > far.Damage);
        Assert.True(close.Damage >= Spec.MaxHealth);
        Assert.True(far.Nx > 0.5);
        Assert.Null(world.Blast(30, 12.5, 0.5, Spec.BombPower, At(36.5), Spec.EyeHeight));
    }

    [Fact]
    public void A_wall_shields_a_player_from_the_blast()
    {
        var world = new World();
        var player = new Hitbox(34.5, 12.5, 0, Spec.PlayerHeight);
        var open = world.Blast(30.5, 12.5, 0.5, Spec.BombPower, player, Spec.EyeHeight)!.Value;
        Assert.Equal(1, world.Exposure(30.5, 12.5, 0.5, player));
        for (var y = 10; y <= 15; y++) for (var z = 0; z <= 3; z++) world.Place(32, y, z - 1, 0, 0, 1, "stone", "p", "a", []);

        var behind = world.Blast(30.5, 12.5, 0.5, Spec.BombPower, player, Spec.EyeHeight)!.Value;

        Assert.Equal(0, world.Exposure(30.5, 12.5, 0.5, player));
        Assert.True(open.Damage > 5);
        Assert.Equal(1, behind.Damage);
    }

    [Fact]
    public void A_parachute_comes_down_two_blocks_a_second_and_rests_on_the_ground()
    {
        var world = new World();
        var z = Spec.DropHeight;
        for (var t = 0; t < Spec.TicksPerSecond; t++) z = Bomb.Descend(world, 30.5, 12.5, z);
        Assert.Equal(Spec.DropHeight - 2, z, 6);

        for (var t = 0; t < 1000; t++) z = Bomb.Descend(world, 30.5, 12.5, z);
        Assert.Equal(0, z);
    }

    [Fact]
    public void A_bomb_lands_on_what_it_meets_and_climbs_out_of_a_block_put_on_it()
    {
        var world = new World();
        var (x, y) = Spec.Trees[0];
        var z = Spec.DropHeight;
        for (var t = 0; t < 1000; t++) z = Bomb.Descend(world, x + 0.5, y + 0.5, z);
        Assert.Equal(7, z);

        world.Place(30, 12, -1, 0, 0, 1, "stone", "p", "a", []);
        Assert.Equal(1, Bomb.Descend(world, 30.5, 12.5, 0.5));
    }

    [Fact]
    public void A_player_picks_up_a_bomb_within_a_block_of_their_hitbox()
    {
        var player = new Hitbox(30, 12, 0, Spec.PlayerHeight);

        Assert.True(Bomb.InPickupReach(player, 31.2, 12, 0));
        Assert.True(Bomb.InPickupReach(player, 30, 12, 2.2));
        Assert.False(Bomb.InPickupReach(player, 31.5, 12, 0));
        Assert.False(Bomb.InPickupReach(player, 30, 12, 2.4));
    }

    [Fact]
    public void A_thrown_bomb_flies_a_parabola_and_explodes_where_it_lands()
    {
        var world = new World();
        double[] p = [10, 12, 1.62], v = [Spec.ThrowSpeed * Math.Cos(0.5), 0, Spec.ThrowSpeed * Math.Sin(0.5)];
        var peak = p[2];
        var flight = Flight.Flying;
        for (var age = 0; flight == Flight.Flying && age < 200; age++)
        {
            flight = Bomb.Fly(world, p, v, Nobody, "me", age);
            peak = Math.Max(peak, p[2]);
        }

        Assert.Equal(Flight.Exploded, flight);
        Assert.True(peak > 3, $"peak {peak}");
        Assert.InRange(p[0], 10 + 12, 10 + 22);
        Assert.InRange(p[2], 0, 0.2);
    }

    [Fact]
    public void A_bomb_hits_a_player_in_its_way_but_not_its_thrower_as_it_leaves_the_hand()
    {
        var world = new World();
        var thrower = ("me", new Hitbox(10, 12, 0, Spec.PlayerHeight));
        var other = ("you", new Hitbox(12, 12, 0, Spec.PlayerHeight));
        double[] p = [10, 12, 1.5], v = [1.0, 0, 0];

        Assert.Equal(Flight.Flying, Bomb.Fly(world, p, v, [thrower, other], "me", 0));
        Assert.Equal(Flight.Exploded, Bomb.Fly(world, p, v, [thrower, other], "me", 1));
        Assert.InRange(p[0], 11.7, 11.8);
    }

    [Fact]
    public void A_bomb_thrown_off_the_edge_of_the_world_is_gone()
    {
        double[] p = [1, 12, 1.5], v = [-1.5, 0, 0];
        Assert.Equal(Flight.Gone, Bomb.Fly(new World(), p, v, Nobody, "me", 0));
    }

    [Fact]
    public void Beyond_five_free_bombs_the_oldest_are_the_surplus()
    {
        var bombs = Enumerable.Range(0, 8).Select(i => new Server.WorldBomb { bomb_id = $"b{i}", state = Bomb.Free, dropped_at = 1000 + i })
            .Append(new Server.WorldBomb { bomb_id = "held", state = Bomb.Held, dropped_at = 1 });

        Assert.Equal(["b2", "b1", "b0"], Bomb.Surplus(bombs, Spec.MaxFreeBombs).Select(b => b.bomb_id));
        Assert.Empty(Bomb.Surplus(bombs.Take(5), Spec.MaxFreeBombs));
    }

    [Fact]
    public void A_bomb_only_moves_forward_through_its_states()
    {
        Assert.True(Bomb.Rank(Bomb.Held) > Bomb.Rank(Bomb.Free));
        Assert.True(Bomb.Rank(Bomb.Flying) > Bomb.Rank(Bomb.Held));
        Assert.True(Bomb.Over(Bomb.Exploded) && Bomb.Over(Bomb.Fizzled));
        Assert.False(Bomb.Over(Bomb.Flying));
    }

    private static Drop.WorldBomb Free(string id, long droppedAt) => new() { bomb_id = id, state = "free", dropped_at = droppedAt, at = droppedAt };

    [Fact]
    public void A_drop_into_a_world_with_five_free_bombs_fizzles_the_oldest()
    {
        var bombs = Enumerable.Range(0, 5).Select(i => Free($"b{i}", 1000 + i))
            .Append(new Drop.WorldBomb { bomb_id = "held", state = "held", dropped_at = 1 });

        var plan = DropBombs.Plan(bombs, 10_000);

        Assert.Equal("b0", Assert.Single(plan.Fizzle).bomb_id);
        Assert.Equal("fizzled", plan.Fizzle[0].state);
        Assert.Equal(5, plan.Free);
        Assert.Equal("free", plan.Drop.state);
        Assert.Equal(DropBombs.DropHeight, plan.Drop.z);
        Assert.InRange(plan.Drop.x, 1, DropBombs.Width - 1);
        Assert.InRange(plan.Drop.y, 1, DropBombs.Depth - 1);
    }

    [Fact]
    public void A_bomb_with_several_rows_is_the_row_furthest_on()
    {
        var bombs = new[]
        {
            Free("b0", 1000),
            new Drop.WorldBomb { bomb_id = "b0", state = "held", dropped_at = 1000, at = 2000 },
            Free("b1", 1500),
            Free("b2", 1600),
            new Drop.WorldBomb { bomb_id = "b2", state = "exploded", dropped_at = 1600, at = 3000 },
        };

        var latest = DropBombs.Latest(bombs).ToDictionary(b => b.bomb_id, b => b.state);
        var plan = DropBombs.Plan(bombs, 10_000);

        Assert.Equal("held", latest["b0"]);
        Assert.Equal("exploded", latest["b2"]);
        Assert.Equal(2, plan.Free);
        Assert.Empty(plan.Fizzle);
    }

    [Fact]
    public void Drops_fall_on_the_quarter_minutes_and_two_fires_drop_the_same_bomb()
    {
        Assert.Equal(15_000, DropBombs.NextSlot(1));
        Assert.Equal(15_000, DropBombs.NextSlot(15_000));
        Assert.Equal(30_000, DropBombs.NextSlot(15_001));

        var one = DropBombs.Plan([], 1_790_601_015_000).Drop;
        var other = DropBombs.Plan([], 1_790_601_015_000).Drop;
        Assert.Equal("drop-1790601015", one.bomb_id);
        Assert.Equal((one.bomb_id, one.x, one.y), (other.bomb_id, other.x, other.y));
        Assert.NotEqual(one.x, DropBombs.Plan([], 1_790_601_030_000).Drop.x);
    }

    [Fact]
    public void A_drop_under_the_limit_fizzles_nothing_and_sweeps_old_finished_bombs()
    {
        var bombs = new[]
        {
            Free("b0", 1000),
            new Drop.WorldBomb { bomb_id = "done", state = "exploded", at = 1000 },
            new Drop.WorldBomb { bomb_id = "recent", state = "exploded", at = 199_000 },
            new Drop.WorldBomb { bomb_id = "lost", state = "flying", at = 100_000 },
        };

        var plan = DropBombs.Plan(bombs, 200_000);

        Assert.Equal("lost", Assert.Single(plan.Fizzle).bomb_id);
        Assert.Equal(["done"], plan.Sweep);
        Assert.Equal(2, plan.Free);
    }

    [Fact]
    public void A_closed_region_puts_out_the_bombs_over_it_and_leaves_the_others_and_the_spent_ones()
    {
        CubeWorld.Server.WorldBomb At(string id, double x, string state) => new() { bomb_id = id, x = x, y = 5, state = state };
        var bombs = new[]
        {
            At("free-here", 30, Bomb.Free), At("held-here", 47.9, Bomb.Held), At("free-left", 23.9, Bomb.Free),
            At("free-right", 48, Bomb.Free), At("gone-here", 30, Bomb.Exploded),
        };

        var put = Bomb.InRegion(bombs, region: 1).Select(b => b.bomb_id).OrderBy(id => id);

        Assert.Equal(["free-here", "held-here"], put);
    }
}
