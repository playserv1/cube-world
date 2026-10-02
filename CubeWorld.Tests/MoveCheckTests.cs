using CubeWorld.Server;
using Xunit;

namespace CubeWorld.Tests;

/// <summary>How far a player's moves may take them (PSV-3000, PSV-2979).</summary>
public class MoveCheckTests
{
    private static MoveCheck At(double x, double y, double z, long now = 0)
    {
        var check = new MoveCheck();
        check.Reset(x, y, z, now);
        return check;
    }

    [Fact]
    public void A_sprint_jump_across_the_world_at_twenty_moves_a_second_is_accepted()
    {
        var check = At(1, 10, 0);
        double x = 1;
        for (long t = 50; x < 69; t += 50)
        {
            x += 7.1 / 20;
            Assert.Equal(MoveVerdict.Accepted, check.Check(x, 10, (t % 600) / 600.0, null, t));
        }
    }

    [Fact]
    public void A_jump_of_forty_blocks_in_one_move_is_refused_and_the_player_stays_where_they_were()
    {
        var check = At(10, 10, 0);
        Assert.Equal(MoveVerdict.Refused, check.Check(50, 10, 0, null, 1000));
        Assert.Equal((10.0, 10.0, 0.0), (check.X, check.Y, check.Z));
        Assert.Equal(1, check.Seq);
    }

    [Fact]
    public void Running_at_forty_blocks_a_second_is_refused_once_the_burst_is_spent()
    {
        var check = At(10, 10, 0);
        double x = 10;
        var refused = false;
        for (long t = 50; t <= 2000 && !refused; t += 50)
        {
            x += 2;
            refused = check.Check(x, 10, 0, null, t) == MoveVerdict.Refused;
        }
        Assert.True(refused);
        Assert.True(check.X < 30);
    }

    [Fact]
    public void A_fall_is_free_and_a_climb_counts()
    {
        Assert.Equal(MoveVerdict.Accepted, At(10, 10, 30).Check(10, 10, 0, null, 50));
        Assert.Equal(MoveVerdict.Refused, At(10, 10, 0).Check(10, 10, 20, null, 50));
    }

    [Fact]
    public void Moves_held_up_by_the_network_and_sent_in_one_burst_are_accepted()
    {
        var check = At(10, 10, 0);
        // A second of sprinting arrives at once, after a second of nothing.
        for (var i = 1; i <= 20; i++) Assert.Equal(MoveVerdict.Accepted, check.Check(10 + i * 0.2806, 10, 0, null, 1000));
    }

    [Fact]
    public void A_knockback_lets_the_player_fly_further_than_they_walk()
    {
        var check = At(10, 10, 0);
        check.Check(10, 10, 0, null, 50);
        Assert.Equal(MoveVerdict.Refused, check.Check(10 + Spec.MoveBurst + 4, 10, 0, null, 100));
        check.Knocked(1.0);
        Assert.Equal(MoveVerdict.Accepted, check.Check(10 + Spec.MoveBurst + 4, 10, 0, null, 150));
    }

    [Fact]
    public void A_client_that_cannot_take_a_correction_is_taken_where_it_says_after_a_second()
    {
        var check = At(10, 10, 0);
        Assert.Equal(MoveVerdict.Refused, check.Check(40, 10, 0, null, 3000));
        Assert.Equal(MoveVerdict.Refused, check.Check(40.2, 10, 0, null, 3500));
        Assert.Equal(MoveVerdict.Accepted, check.Check(40.4, 10, 0, null, 4000));
        Assert.Equal(40.4, check.X);

        var numbered = At(10, 10, 0);
        Assert.Equal(MoveVerdict.Refused, numbered.Check(40, 10, 0, 0, 3000));
        Assert.Equal(MoveVerdict.Refused, numbered.Check(40.4, 10, 0, 1, 4000));
    }

    [Fact]
    public void Moves_sent_before_the_client_took_a_correction_are_dropped_and_its_next_ones_are_checked()
    {
        var check = At(10, 10, 0);
        Assert.Equal(MoveVerdict.Refused, check.Check(60, 10, 0, 0, 50));
        Assert.Equal(MoveVerdict.Stale, check.Check(61, 10, 0, 0, 100));
        Assert.Equal(MoveVerdict.Accepted, check.Check(10.2, 10, 0, 1, 150));
        Assert.Equal(MoveVerdict.Refused, check.Check(60, 10, 0, 1, 200));
        Assert.Equal(2, check.Seq);
    }
}
