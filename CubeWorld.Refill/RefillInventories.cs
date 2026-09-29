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
    private const int StackSize = 64;
    private static readonly string[] Kinds = ["grass", "dirt", "sand", "stone", "wood", "brick", "glass", "gold", "leaves"];

    private ITable<CubeInventory> Inventories => Platform.Table<CubeInventory>();

    protected override async Task<FunctionResponse> HandleAsync(object _, CancellationToken ct)
    {
        foreach (var player in await LoadInventories(ct))
        {
            var stacks = ReadStacks(player);
            GiveOneOfEachKind(stacks);
            await Save(player, stacks, ct);
        }

        return FunctionResponse.Json(new { ok = true });
    }

    private async Task<List<CubeInventory>> LoadInventories(CancellationToken ct)
    {
        var rows = await Inventories.Query().Take(200).ToListAsync(ct);
        return rows.Select(r => r.Fields!).ToList();
    }

    /// How many blocks of each kind the player has, e.g. { "sand": 12, "stone": 64 }.
    private static Dictionary<string, int> ReadStacks(CubeInventory player)
    {
        if (string.IsNullOrEmpty(player.stacks))
        {
            return new Dictionary<string, int>();
        }

        return JsonSerializer.Deserialize<Dictionary<string, int>>(player.stacks)!;
    }

    private static void GiveOneOfEachKind(Dictionary<string, int> stacks)
    {
        foreach (var kind in Kinds)
        {
            stacks[kind] = Math.Min(StackSize, stacks.GetValueOrDefault(kind) + 1);
        }
    }

    private async Task Save(CubeInventory player, Dictionary<string, int> stacks, CancellationToken ct)
    {
        player.stacks = JsonSerializer.Serialize(stacks);
        player.cubes = stacks.Values.Sum();
        await Inventories.UpsertByAsync(i => i.player_id, player.player_id, UpsertMode.Managed, player, ct);
    }
}
