using CubeWorld.Server;
using Xunit;

namespace CubeWorld.Tests;

public class BombTests
{
    private static readonly (string, Hitbox)[] Nobody = [];

    [Fact]
    public void An_explosion_breaks_the_block_under_it_and_one_around_but_no_deeper()
    {
        var world = new World();

        var update = world.Explode(30.5, 12.5, 0.3, Spec.CraterPower, new Random(1), "p", "a");

        Assert.Equal("air", world.KindAt(30, 12, -1));
        Assert.Equal("air", world.KindAt(31, 13, -1));
        Assert.NotEqual("air", world.KindAt(30, 12, -2));
        Assert.Equal("grass", world.KindAt(32, 12, -1));
        Assert.All(update.Changes, c => Assert.Equal("air", c.Cube.kind));
        Assert.Equal(9, update.Changes.Count);
    }

    [Fact]
    public void A_blast_on_a_border_breaks_only_the_blocks_of_the_region_it_is_worked_out_for()
    {
        var world = new World();

        var update = world.Explode(World.RegionSize, 12.5, 0, Spec.CraterPower, new Random(1), "p", "a", region: 0);

        Assert.NotEmpty(update.Changes);
        Assert.All(update.Changes, c => Assert.Equal(0, World.RegionOf(c.Cube.x)));
        Assert.Equal("air", world.KindAt(World.RegionSize - 1, 12, -1));
        Assert.Equal("grass", world.KindAt(World.RegionSize, 12, -1));
    }

    [Fact]
    public void Two_servers_working_out_one_blast_from_its_seed_break_together_what_one_world_would()
    {
        var seed = Bomb.BlastSeed("bomb-7");
        HashSet<(string, string)> Broken(int? region) =>
            new World().Explode(World.RegionSize, 12.5, 0, Spec.CraterPower, new Random(seed), "p", "a", region)
                .Changes.Select(c => (c.Cube.key, c.Cube.kind)).ToHashSet();

        var whole = Broken(null);
        var left = Broken(0);
        var right = Broken(1);

        Assert.NotEmpty(left);
        Assert.NotEmpty(right);
        Assert.Empty(left.Intersect(right));
        Assert.Equal(whole, left.Union(right).ToHashSet());
    }

    [Fact]
    public void A_bomb_blasts_with_the_same_seed_on_every_server()
    {
        Assert.Equal(1872573689, Bomb.BlastSeed("bomb-7"));
        Assert.NotEqual(Bomb.BlastSeed("bomb-7"), Bomb.BlastSeed("bomb-8"));
    }

    [Fact]
    public void A_blast_breaks_stone_glass_and_gold_at_the_first_go_as_it_does_dirt()
    {
        foreach (var kind in new[] { "dirt", "grass", "glass", "stone", "brick", "gold", "wood", "leaves" })
        {
            var world = new World();
            world.Place(29, 12, -1, 0, 0, 1, kind, "p", "a", []);
            world.Place(31, 12, -1, 0, 0, 1, kind, "p", "a", []);

            world.Explode(30.5, 12.5, 0.5, Spec.CraterPower, new Random(1), "p", "a");

            Assert.Equal("air", world.KindAt(29, 12, 0));
            Assert.Equal("air", world.KindAt(31, 12, 0));
        }
    }

    [Fact]
    public void Sand_over_a_crater_falls_into_it()
    {
        var world = new World();
        for (var z = -1; z < 5; z++) world.Place(30, 12, z, 0, 0, 1, "sand", "p", "a", []);

        var update = world.Explode(31.5, 12.5, 0.3, Spec.CraterPower, new Random(1), "p", "a");

        Assert.NotEmpty(update.Falls);
        Assert.True(world.IsSolid(30, 12, 0));
    }

    [Fact]
    public void A_blast_hurts_most_up_close_and_not_at_all_beyond_its_reach()
    {
        var world = new World();
        Hitbox At(double x) => new(x, 12.5, 0, Spec.PlayerHeight);

        var close = world.Blast(30, 12.5, 0.5, Spec.BombPower, At(30.5), Spec.EyeHeight)!.Value;
        var far = world.Blast(30, 12.5, 0.5, Spec.BombPower, At(32.5), Spec.EyeHeight)!.Value;

        Assert.True(close.Damage > far.Damage);
        Assert.True(close.Damage >= Spec.MaxHealth);
        Assert.True(far.Nx > 0.5);
        Assert.Null(world.Blast(30, 12.5, 0.5, Spec.BombPower, At(33.1), Spec.EyeHeight));
    }

    [Fact]
    public void A_wall_shields_a_player_from_the_blast()
    {
        var world = new World();
        var player = new Hitbox(32.5, 12.5, 0, Spec.PlayerHeight);
        var open = world.Blast(30.5, 12.5, 0.5, Spec.BombPower, player, Spec.EyeHeight)!.Value;
        Assert.Equal(1, world.Exposure(30.5, 12.5, 0.5, player));
        for (var y = 10; y <= 15; y++) for (var z = 0; z <= 3; z++) world.Place(31, y, z - 1, 0, 0, 1, "stone", "p", "a", []);

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
    public void A_bomb_only_moves_forward_through_its_states()
    {
        Assert.True(Bomb.Rank(Bomb.Held) > Bomb.Rank(Bomb.Free));
        Assert.True(Bomb.Rank(Bomb.Flying) > Bomb.Rank(Bomb.Held));
        Assert.True(Bomb.Over(Bomb.Exploded) && Bomb.Over(Bomb.Fizzled));
        Assert.False(Bomb.Over(Bomb.Flying));
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
