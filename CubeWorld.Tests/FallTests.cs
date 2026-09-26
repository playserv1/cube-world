using CubeWorld.Server;
using Xunit;

namespace CubeWorld.Tests;

public class FallTests
{
    [Fact]
    public void A_fall_hurts_by_the_height_it_came_from_less_three_blocks()
    {
        var fall = new PlayerFall();
        Assert.Equal(0, fall.Step(10, onGround: false));
        Assert.Equal(0, fall.Step(4, onGround: false));
        Assert.Equal(7, fall.Step(0, onGround: true));
        Assert.False(fall.Airborne);
    }

    [Fact]
    public void A_short_drop_does_not_hurt()
    {
        var fall = new PlayerFall();
        fall.Step(2.5, onGround: false);
        Assert.Equal(0, fall.Step(0, onGround: true));
    }

    [Fact]
    public void A_fall_that_began_on_the_old_server_hurts_on_the_next_one()
    {
        // The new server first hears the player halfway down; the client says it left the ground ten blocks up.
        var fall = new PlayerFall();
        fall.Step(5, onGround: false, saidPeak: 10);
        Assert.Equal(7, fall.Step(0, onGround: true));
    }

    [Fact]
    public void A_client_cannot_make_a_fall_shorter_by_saying_so()
    {
        var fall = new PlayerFall();
        fall.Step(10, onGround: false);
        fall.Step(5, onGround: false, saidPeak: 1);
        Assert.Equal(7, fall.Step(0, onGround: true));
    }

    [Fact]
    public void A_peak_above_the_world_is_taken_as_the_top_of_it()
    {
        var fall = new PlayerFall();
        fall.Step(5, onGround: false, saidPeak: 10_000);
        Assert.Equal(World.MaxZ + 8, fall.Peak);
    }

    [Fact]
    public void Standing_on_the_ground_is_no_fall()
    {
        var fall = new PlayerFall();
        Assert.Equal(0, fall.Step(0, onGround: true, saidPeak: 30));
    }
}
