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

public sealed record RefillRequest(int? Amount);

/// <summary>Once a minute, every player gets a few more of each block kind, up to a full stack of 64.</summary>
public sealed class RefillInventories : PlatformFunction<RefillRequest>
{
    private const int StackSize = 64;
    private static readonly string[] Kinds = ["grass", "dirt", "sand", "stone", "wood", "brick", "glass", "gold"];

    protected override async Task<FunctionResponse> HandleAsync(RefillRequest body, CancellationToken ct)
    {
        var amount = body.Amount ?? 1;
        var inventories = Platform.Table<CubeInventory>();
        var refilled = 0;

        foreach (var row in await inventories.Query().Where(i => i.cubes < StackSize * Kinds.Length).Take(200).ToListAsync(ct))
        {
            var inventory = row.Fields!;
            Dictionary<string, int> stacks;
            try { stacks = JsonSerializer.Deserialize<Dictionary<string, int>>(inventory.stacks) ?? new(); }
            catch (JsonException) { stacks = new(); }

            foreach (var kind in Kinds) stacks[kind] = Math.Min(StackSize, stacks.GetValueOrDefault(kind) + amount);
            inventory.stacks = JsonSerializer.Serialize(stacks);
            inventory.cubes = stacks.Values.Sum();
            await inventories.UpsertByAsync(i => i.player_id, inventory.player_id, UpsertMode.Managed, inventory, ct);
            refilled++;
        }

        await Platform.Log($"refilled {refilled} inventories by {amount} of each kind", ct: ct);
        return FunctionResponse.Json(new { refilled, amount });
    }
}
