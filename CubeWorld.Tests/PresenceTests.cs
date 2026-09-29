using CubeWorld.Server;
using Xunit;

namespace CubeWorld.Tests;

public class PresenceTests
{
    private const long Now = 1_000_000;

    private static WorldPresence SpawnOnBlue() => new()
    {
        player_id = "p", name = "walker", server = "hywr1", color = "blue", x = 36, y = 12, z = 0, health = Spec.MaxHealth,
    };

    private static WorldPresence SeenOnRed(long seenAt, double health = 13) => new()
    {
        player_id = "p", name = "walker", server = "4qdcb", color = "red", x = 25.1, y = 6, z = 1,
        yaw = -1.5, pitch = 0.2, health = health, sneaking = 1, sprinting = 0, seen_at = seenAt,
    };

    [Fact]
    public void A_player_walking_over_a_border_does_not_jump_to_the_middle_of_the_new_region()
    {
        var pose = WorldPresence.Arriving(SpawnOnBlue(), SeenOnRed(Now - 200), Now);

        Assert.Equal((25.1, 6.0, 1.0), (pose.x, pose.y, pose.z));
        Assert.Equal((-1.5, 0.2, 1), (pose.yaw, pose.pitch, pose.sneaking));
        Assert.Equal(("hywr1", "blue"), (pose.server, pose.color));
    }

    [Fact]
    public void A_player_walking_over_a_border_keeps_their_health()
    {
        Assert.Equal(13, WorldPresence.Arriving(SpawnOnBlue(), SeenOnRed(Now - 200), Now).health);
    }

    [Fact]
    public void The_new_server_does_not_announce_the_pose_it_heard_again_as_if_it_were_new()
    {
        Assert.Equal(Now - 200, WorldPresence.Arriving(SpawnOnBlue(), SeenOnRed(Now - 200), Now).seen_at);
    }

    [Fact]
    public void A_player_nobody_has_seen_lately_starts_at_the_spawn()
    {
        Assert.Equal(36, WorldPresence.Arriving(SpawnOnBlue(), null, Now).x);
        Assert.Equal(36, WorldPresence.Arriving(SpawnOnBlue(), SeenOnRed(Now - 5000), Now).x);
    }

    [Fact]
    public void A_player_who_died_on_the_other_server_starts_at_the_spawn()
    {
        var pose = WorldPresence.Arriving(SpawnOnBlue(), SeenOnRed(Now - 200, health: 0), Now);

        Assert.Equal((36.0, Spec.MaxHealth), (pose.x, pose.health));
    }
}
