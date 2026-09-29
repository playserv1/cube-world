using System.Text.Json;
using PlayServ.Sdk;
using PlayServ.Sdk.Data;

namespace CubeWorld.Refill;

[EntityName("CubeInventory")]
public sealed class CubeInventory
{
    public string player_id { get; set; } = "";
    public int cubes { get; set; }
    public string stacks { get; set; } = "";
}

/// Every minute: every player gets one more block of each kind, up to 64.
public sealed class RefillInventories : PlatformFunction<object>
{
    static readonly string[] Kinds = ["grass", "dirt", "sand", "stone", "wood", "brick", "glass", "gold", "leaves"];

    protected override async Task<FunctionResponse> HandleAsync(object _, CancellationToken ct)
    {
        var inventories = Platform.Table<CubeInventory>();

        foreach (var row in await inventories.Query().Take(200).ToListAsync(ct))
        {
            var player = row.Fields!;
            var stacks = string.IsNullOrEmpty(player.stacks)
                ? new Dictionary<string, int>()
                : JsonSerializer.Deserialize<Dictionary<string, int>>(player.stacks)!;

            foreach (var kind in Kinds)
                stacks[kind] = Math.Min(64, stacks.GetValueOrDefault(kind) + 1);

            player.stacks = JsonSerializer.Serialize(stacks);
            player.cubes = stacks.Values.Sum();
            await inventories.UpsertByAsync(i => i.player_id, player.player_id, UpsertMode.Managed, player, ct);
        }

        return FunctionResponse.Json(new { ok = true });
    }
}
