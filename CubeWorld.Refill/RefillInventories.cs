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
    private const int StackSize = 64, PageSize = 200;
    private static readonly string[] Kinds = ["grass", "dirt", "sand", "stone", "wood", "brick", "glass", "gold", "leaves"];

    private ITable<CubeInventory> Inventories => Platform.Table<CubeInventory>();

    protected override async Task<FunctionResponse> HandleAsync(object _, CancellationToken ct)
    {
        var refilled = 0;
        foreach (var listed in await LoadInventories(ct))
        {
            if (Same(ReadStacks(listed), Refilled(ReadStacks(listed)))) continue;
            // The pages were read a while ago: the row is read again right before the write, so a server's write since
            // (a block placed, a crossing) is topped up instead of overwritten.
            if ((await Inventories.FindByAsync(i => i.player_id, listed.player_id, ct))?.Fields is not { } player) continue;
            var before = ReadStacks(player);
            var after = Refilled(before);
            if (Same(before, after)) continue;
            await Save(player, after, ct);
            refilled++;
        }

        return FunctionResponse.Json(new { ok = true, refilled });
    }

    /// Every row, a page at a time and all of them before the first write: the table holds a row per player who ever
    /// played, far more than one page, and a row written while the pages are read would move under the cursor.
    private async Task<List<CubeInventory>> LoadInventories(CancellationToken ct)
    {
        var rows = new List<CubeInventory>();
        string? cursor = null;
        do
        {
            var page = await Inventories.Query().OrderBy(i => i.player_id).Take(PageSize).WithCursor(cursor).ToPageAsync(ct);
            rows.AddRange(page.Items.Where(r => r.Fields is not null).Select(r => r.Fields!));
            cursor = page.NextCursor;
        } while (!string.IsNullOrEmpty(cursor));
        return rows;
    }

    /// How many blocks of each kind the player has, e.g. { "sand": 12, "stone": 64 }.
    private static Dictionary<string, int> ReadStacks(CubeInventory player)
    {
        if (string.IsNullOrEmpty(player.stacks))
        {
            return new Dictionary<string, int>();
        }

        try
        {
            return JsonSerializer.Deserialize<Dictionary<string, int>>(player.stacks) ?? new Dictionary<string, int>();
        }
        catch (JsonException)
        {
            return new Dictionary<string, int>();
        }
    }

    /// <summary>
    /// The stacks after one refill: one more of each kind, up to a stack, under the kinds' own names. A row an Unreal
    /// server wrote names stone "Stone": that is stone, and a "stone" an earlier refill added beside it folds into it
    /// (the larger count is the player's). Kinds this function does not know are kept as they are.
    /// </summary>
    public static Dictionary<string, int> Refilled(IReadOnlyDictionary<string, int> stacks)
    {
        var result = new Dictionary<string, int>();
        foreach (var (key, count) in stacks)
        {
            var kind = Kinds.FirstOrDefault(k => string.Equals(k, key, StringComparison.OrdinalIgnoreCase)) ?? key;
            result[kind] = Math.Max(result.GetValueOrDefault(kind), count);
        }

        foreach (var kind in Kinds)
        {
            result[kind] = Math.Min(StackSize, result.GetValueOrDefault(kind) + 1);
        }

        return result;
    }

    /// The same keys with the same counts: a full inventory needs no write.
    public static bool Same(IReadOnlyDictionary<string, int> a, IReadOnlyDictionary<string, int> b) =>
        a.Count == b.Count && a.All(pair => b.TryGetValue(pair.Key, out var count) && count == pair.Value);

    private async Task Save(CubeInventory player, Dictionary<string, int> stacks, CancellationToken ct)
    {
        player.stacks = JsonSerializer.Serialize(stacks);
        player.cubes = stacks.Values.Sum();
        await Inventories.UpsertByAsync(i => i.player_id, player.player_id, UpsertMode.Managed, player, ct);
    }
}
