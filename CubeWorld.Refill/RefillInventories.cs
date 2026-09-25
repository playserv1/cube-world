using PlayServ.Sdk;
using PlayServ.Sdk.Data;

namespace CubeWorld.Refill;

[EntityName("CubeInventory")]
public sealed class CubeInventory
{
    public string player_id { get; set; } = "";
    public int cubes { get; set; }
}

public sealed record RefillRequest(int? Amount);

public sealed class RefillInventories : PlatformFunction<RefillRequest>
{
    private const int MaxCubes = 10;

    protected override async Task<FunctionResponse> HandleAsync(RefillRequest body, CancellationToken ct)
    {
        var amount = body.Amount ?? 3;
        var inventories = Platform.Table<CubeInventory>();
        var refilled = 0;

        foreach (var row in await inventories.Query().Where(i => i.cubes < MaxCubes).Take(200).ToListAsync(ct))
        {
            var inventory = row.Fields!;
            inventory.cubes = Math.Min(MaxCubes, inventory.cubes + amount);
            await inventories.UpsertByAsync(i => i.player_id, inventory.player_id, UpsertMode.Managed, inventory, ct);
            refilled++;
        }

        await Platform.Log($"refilled {refilled} inventories by {amount}", ct: ct);
        return FunctionResponse.Json(new { refilled, amount });
    }
}
