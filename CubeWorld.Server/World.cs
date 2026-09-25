using PlayServ.Sdk.Data;

namespace CubeWorld.Server;

[EntityName("WorldCube")]
public sealed class WorldCube
{
    public string key { get; set; } = "";
    public int x { get; set; }
    public int y { get; set; }
    public int z { get; set; }
    public string kind { get; set; } = "";
    public string placed_by { get; set; } = "";
    public string placed_on { get; set; } = "";
}

[EntityName("CubeInventory")]
public sealed class CubeInventory
{
    public string player_id { get; set; } = "";
    public int cubes { get; set; }
}

[EntityName("WorldPresence")]
public sealed class WorldPresence
{
    public string player_id { get; set; } = "";
    public string name { get; set; } = "";
    public string server { get; set; } = "";
    public string color { get; set; } = "";
    public double x { get; set; }
    public double y { get; set; }
    public double z { get; set; }
    public double yaw { get; set; }
    public long seen_at { get; set; }
}

[EntityName("WorldRegion")]
public sealed class WorldRegion
{
    public string region { get; set; } = "";
    public string server { get; set; } = "";
    public string color { get; set; } = "";
    public string room { get; set; } = "";
    public long seen_at { get; set; }
}

public sealed class World
{
    public const int RegionSize = 24, Width = RegionSize * 3, Depth = RegionSize, Height = 8;
    public static readonly string[] Kinds = ["grass", "stone", "wood", "brick", "glass", "gold"];
    public static readonly string[] RegionColors = ["red", "blue", "green"];

    private readonly Dictionary<string, WorldCube> _cubes = new();

    public IEnumerable<WorldCube> Cubes => _cubes.Values;

    public WorldCube? Place(int x, int y, string kind, string by, string on)
    {
        var z = Enumerable.Range(0, Height).FirstOrDefault(h => !_cubes.ContainsKey(Key(x, y, h)), -1);
        if (!Inside(x, y) || z < 0 || !Kinds.Contains(kind)) return null;
        var cube = new WorldCube { key = Key(x, y, z), x = x, y = y, z = z, kind = kind, placed_by = by, placed_on = on };
        return _cubes[cube.key] = cube;
    }

    public WorldCube? Break(int x, int y)
    {
        var top = _cubes.Values.Where(c => c.x == x && c.y == y).MaxBy(c => c.z);
        if (top is not null) _cubes.Remove(top.key);
        return top;
    }

    public bool Apply(string op, WorldCube cube)
    {
        if (op == "delete") return _cubes.Remove(cube.key);
        if (!Inside(cube.x, cube.y) || _cubes.TryGetValue(cube.key, out var known) && known.kind == cube.kind) return false;
        _cubes[cube.key] = cube;
        return true;
    }

    public void Load(IEnumerable<WorldCube> cubes)
    {
        foreach (var cube in cubes) _cubes[cube.key] = cube;
    }

    private static bool Inside(int x, int y) => x is >= 0 and < Width && y is >= 0 and < Depth;

    private static string Key(int x, int y, int z) => $"{x}:{y}:{z}";
}
