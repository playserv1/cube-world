using CubeWorld.Server;
using Xunit;

namespace CubeWorld.Tests;

/// <summary>Two writers of one player's inventory row across a crossing, and the refill function: nothing undone, nothing lost.</summary>
public class InventorySyncTests
{
    private static Dictionary<string, int> Stacks(int dirt, int stone = 10) => new() { ["dirt"] = dirt, ["stone"] = stone };

    [Fact]
    public void A_block_spent_on_the_old_server_just_before_the_crossing_stays_spent()
    {
        // The new server read the row before the old server's last write landed: dirt 10. That write, dirt 9, comes after.
        var sync = new InventorySync(Stacks(10));
        var merged = sync.Heard(ours: Stacks(10), theirs: Stacks(9));
        Assert.Equal(9, merged!["dirt"]);
    }

    [Fact]
    public void What_the_player_did_here_meanwhile_is_kept_too()
    {
        // Read dirt 10, the player placed 2 here (8), then the old server's last write says it spent one there (9).
        var sync = new InventorySync(Stacks(10));
        var merged = sync.Heard(ours: Stacks(8), theirs: Stacks(9));
        Assert.Equal(7, merged!["dirt"]);
        Assert.Equal(10, merged["stone"]);
    }

    [Fact]
    public void This_servers_own_write_coming_back_changes_nothing()
    {
        var sync = new InventorySync(Stacks(10));
        sync.Wrote(Stacks(9));
        Assert.Null(sync.Heard(ours: Stacks(9), theirs: Stacks(9)));
        Assert.Equal(9, sync.Base["dirt"]);
    }

    [Fact]
    public void Only_the_latest_of_several_own_writes_may_come_back()
    {
        var sync = new InventorySync(Stacks(10));
        sync.Wrote(Stacks(9));
        sync.Wrote(Stacks(8));
        Assert.Null(sync.Heard(ours: Stacks(8), theirs: Stacks(8)));
        // The earlier one is behind us: the same row heard again now is someone else's.
        Assert.NotNull(sync.Heard(ours: Stacks(8), theirs: Stacks(9)));
    }

    [Fact]
    public void A_refill_tops_up_what_the_player_holds_now()
    {
        var sync = new InventorySync(Stacks(10));
        sync.Wrote(Stacks(9));
        sync.Heard(ours: Stacks(9), theirs: Stacks(9));
        // The player spent another one, not yet heard back, when the refill (read at 9) wrote 10 and 11.
        sync.Wrote(Stacks(8));
        var merged = sync.Heard(ours: Stacks(8), theirs: Stacks(10, 11));
        Assert.Equal((9, 11), (merged!["dirt"], merged["stone"]));
    }

    [Fact]
    public void A_merge_keeps_every_kind_within_zero_and_a_stack()
    {
        var merged = InventorySync.Merge(ours: Stacks(64, 0), @base: Stacks(10, 5), theirs: Stacks(20, 1));
        Assert.Equal((Spec.StackSize, 0), (merged["dirt"], merged["stone"]));
    }

    [Fact]
    public void A_kind_only_one_side_lists_counts_as_none_on_the_other()
    {
        var merged = InventorySync.Merge(ours: new Dictionary<string, int> { ["dirt"] = 3 }, @base: new Dictionary<string, int>(), theirs: new Dictionary<string, int> { ["gold"] = 2 });
        Assert.Equal((3, 2), (merged["dirt"], merged["gold"]));
    }
}
