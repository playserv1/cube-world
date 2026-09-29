using System.Text.Json;
using PlayServ.Sdk.Data;

namespace CubeWorld.Server;

// Storage and protocol keep x, y on the ground and z up; the browser client maps z to its Y axis.

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
    public string stacks { get; set; } = "";
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
    public double pitch { get; set; }
    public double health { get; set; } = Spec.MaxHealth;
    public int sneaking { get; set; }
    public int sprinting { get; set; }
    public long seen_at { get; set; }
}

/// <summary>A hit on a player another server hosts: written by the attacker's server, applied by the victim's.</summary>
[EntityName("WorldHit")]
public sealed class WorldHit
{
    public string hit_id { get; set; } = "";
    public string victim { get; set; } = "";
    public string attacker { get; set; } = "";
    public double damage { get; set; }
    public double kx { get; set; }
    public double ky { get; set; }
    public double strength { get; set; }
    public long at { get; set; }
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

public sealed record Change(string Op, WorldCube Cube);

public sealed record Fall(string Kind, int X, int Y, int FromZ, int ToZ);

/// <summary>A player's hitbox: feet at (X, Y, Z), <see cref="Spec.PlayerWidth"/> wide, <paramref name="Height"/> tall.</summary>
public sealed record Hitbox(double X, double Y, double Z, double Height);

public sealed class WorldUpdate
{
    public List<Change> Changes { get; } = [];
    public List<Fall> Falls { get; } = [];
}

/// <summary>
/// The world: a superflat terrain generated on the fly plus the records that override it. A record with kind
/// "air" is a generated block someone dug out. Blocks are placed against a face and broken one at a time;
/// blocks affected by gravity fall when nothing holds them.
/// </summary>
public sealed class World
{
    public const int RegionSize = 24, Width = RegionSize * 3, Depth = RegionSize, MinZ = -4, MaxZ = 64;
    public static readonly string[] RegionColors = ["red", "blue", "green"];

    private readonly Dictionary<string, WorldCube> _overrides = new();

    public IEnumerable<WorldCube> Overrides => _overrides.Values;

    /// <summary>The region a column belongs to: region r is the columns r·<see cref="RegionSize"/> up to the next region's first.</summary>
    public static int RegionOf(double x) => (int)Math.Floor(x / RegionSize);

    /// <summary>The columns of <paramref name="region"/>: <c>From</c> is its first, <c>To</c> the next region's first.</summary>
    public static (int From, int To) Columns(int region) => (region * RegionSize, (region + 1) * RegionSize);

    public static bool Inside(int x, int y, int z) => x is >= 0 and < Width && y is >= 0 and < Depth && z is >= MinZ and < MaxZ;

    public static string Generated(int x, int y, int z)
    {
        if (!Inside(x, y, z)) return "air";
        if (z >= 0) return Spec.TreeBlocks.GetValueOrDefault(Key(x, y, z), "air");
        foreach (var layer in Spec.Layers) if (layer.z == z) return layer.kind;
        return "air";
    }

    public string KindAt(int x, int y, int z) => _overrides.TryGetValue(Key(x, y, z), out var cube) ? cube.kind : Generated(x, y, z);

    public Block BlockAt(int x, int y, int z) => Spec.Of(KindAt(x, y, z));

    public bool IsSolid(int x, int y, int z) => Inside(x, y, z) && BlockAt(x, y, z).Solid;

    /// <summary>Places <paramref name="kind"/> against the face (nx, ny, nz) of the block at (ax, ay, az).</summary>
    public WorldUpdate? Place(int ax, int ay, int az, int nx, int ny, int nz, string kind, string by, string on, IEnumerable<Hitbox> players)
    {
        if (!Spec.Of(kind).Placeable || !IsSolid(ax, ay, az) || Math.Abs(nx) + Math.Abs(ny) + Math.Abs(nz) != 1) return null;
        int x = ax + nx, y = ay + ny, z = az + nz;
        if (!Inside(x, y, z) || BlockAt(x, y, z).Solid || players.Any(p => Intersects(p, x, y, z))) return null;

        var update = new WorldUpdate();
        Set(x, y, z, kind, by, on, update);
        Settle(x, y, z, update);
        return update;
    }

    public (WorldUpdate Update, Block Broken)? Break(int x, int y, int z, string by, string on)
    {
        var block = BlockAt(x, y, z);
        if (!Inside(x, y, z) || !block.Solid || !block.Breakable) return null;

        var update = new WorldUpdate();
        Set(x, y, z, "air", by, on, update);
        Settle(x, y, z, update);
        return (update, block);
    }

    /// <summary>A change another server wrote. Returns whether it changed anything here.</summary>
    public bool Apply(string op, WorldCube cube)
    {
        if (op == "delete") return _overrides.Remove(cube.key);
        if (!Inside(cube.x, cube.y, cube.z) || _overrides.TryGetValue(cube.key, out var known) && known.kind == cube.kind) return false;
        _overrides[cube.key] = cube;
        return true;
    }

    public void Load(IEnumerable<WorldCube> cubes)
    {
        foreach (var cube in cubes) _overrides[cube.key] = cube;
    }

    /// <summary>
    /// An explosion as Minecraft's: rays go out from the centre towards every point of a 16 × 16 × 16 cube's
    /// surface, each with an intensity of power × (0.7 to 1.3). Every 0.3 blocks a ray loses 0.225 and, in a
    /// block, (blast resistance + 0.3) × 0.3; a block the ray still has intensity for is destroyed. Nothing drops.
    /// </summary>
    public WorldUpdate Explode(double cx, double cy, double cz, double power, Random random, string by, string on)
    {
        var destroyed = new HashSet<(int x, int y, int z)>();
        for (var i = 0; i < 16; i++)
            for (var j = 0; j < 16; j++)
                for (var k = 0; k < 16; k++)
                {
                    if (i is not (0 or 15) && j is not (0 or 15) && k is not (0 or 15)) continue;
                    double dx = i / 15.0 * 2 - 1, dy = j / 15.0 * 2 - 1, dz = k / 15.0 * 2 - 1;
                    var length = Math.Sqrt(dx * dx + dy * dy + dz * dz);
                    dx /= length; dy /= length; dz /= length;
                    double x = cx, y = cy, z = cz;
                    for (var intensity = power * (0.7 + random.NextDouble() * 0.6); intensity > 0; intensity -= 0.22500001)
                    {
                        int bx = (int)Math.Floor(x), by2 = (int)Math.Floor(y), bz = (int)Math.Floor(z);
                        var block = BlockAt(bx, by2, bz);
                        if (Inside(bx, by2, bz) && block.Solid)
                        {
                            intensity -= (block.BlastResistance + 0.3) * 0.3;
                            if (intensity > 0 && block.Breakable) destroyed.Add((bx, by2, bz));
                        }
                        x += dx * 0.3; y += dy * 0.3; z += dz * 0.3;
                    }
                }

        var update = new WorldUpdate();
        foreach (var (x, y, z) in destroyed.OrderBy(b => b.z)) Set(x, y, z, "air", by, on, update);
        foreach (var column in destroyed.GroupBy(b => (b.x, b.y))) Settle(column.Key.x, column.Key.y, column.Min(b => b.z), update);
        return update;
    }

    /// <summary>
    /// What an explosion does to a player, as Minecraft works it out: within twice the power, impact is
    /// (1 − distance / (2 × power)) × the share of the hitbox the centre can see; damage is
    /// ⌊(impact² + impact) / 2 × 7 × 2 × power + 1⌋ and the player is thrown away from the centre with the impact.
    /// </summary>
    public (double Damage, double Nx, double Ny, double Impact)? Blast(double cx, double cy, double cz, double power, Hitbox p, double eye)
    {
        double fx = p.X - cx, fy = p.Y - cy, fz = p.Z - cz;
        var distance = Math.Sqrt(fx * fx + fy * fy + fz * fz) / (2 * power);
        if (distance > 1) return null;
        double dx = p.X - cx, dy = p.Y - cy, dz = p.Z + eye - cz;
        var length = Math.Sqrt(dx * dx + dy * dy + dz * dz);
        if (length < 1e-9) (dx, dy, length) = (0, 0, 1);
        var impact = (1 - distance) * Exposure(cx, cy, cz, p);
        var damage = Math.Floor((impact * impact + impact) / 2 * 7 * (2 * power) + 1);
        return (damage, dx / length, dy / length, impact);
    }

    /// <summary>The share of points spread through the hitbox from which the centre is in plain sight.</summary>
    public double Exposure(double cx, double cy, double cz, Hitbox p)
    {
        var half = Spec.PlayerWidth / 2;
        double sx = 1 / (Spec.PlayerWidth * 2 + 1), sz = 1 / (p.Height * 2 + 1);
        var offset = (1 - Math.Floor(1 / sx) * sx) / 2;
        int seen = 0, all = 0;
        for (var a = 0.0; a <= 1; a += sx)
            for (var b = 0.0; b <= 1; b += sx)
                for (var c = 0.0; c <= 1; c += sz)
                {
                    all++;
                    double x = p.X - half + a * Spec.PlayerWidth + offset, y = p.Y - half + b * Spec.PlayerWidth + offset, z = p.Z + c * p.Height;
                    if (Clear(x, y, z, cx, cy, cz)) seen++;
                }
        return all == 0 ? 0 : seen / (double)all;
    }

    private bool Clear(double x, double y, double z, double tx, double ty, double tz)
    {
        double dx = tx - x, dy = ty - y, dz = tz - z;
        var steps = (int)Math.Ceiling(Math.Sqrt(dx * dx + dy * dy + dz * dz) / 0.1);
        for (var i = 0; i < steps; i++)
        {
            var t = i / (double)steps;
            if (IsSolid((int)Math.Floor(x + dx * t), (int)Math.Floor(y + dy * t), (int)Math.Floor(z + dz * t))) return false;
        }
        return true;
    }

    public static bool Intersects(Hitbox p, int x, int y, int z)
    {
        var half = Spec.PlayerWidth / 2;
        return p.X - half < x + 1 && p.X + half > x && p.Y - half < y + 1 && p.Y + half > y && p.Z < z + 1 && p.Z + p.Height > z;
    }

    /// <summary>Distance from a point to the nearest point of the block at (x, y, z).</summary>
    public static double DistanceToBlock(double px, double py, double pz, int x, int y, int z)
    {
        double dx = Math.Max(0, Math.Max(x - px, px - (x + 1)));
        double dy = Math.Max(0, Math.Max(y - py, py - (y + 1)));
        double dz = Math.Max(0, Math.Max(z - pz, pz - (z + 1)));
        return Math.Sqrt(dx * dx + dy * dy + dz * dz);
    }

    public static double DistanceToHitbox(double px, double py, double pz, Hitbox h)
    {
        var half = Spec.PlayerWidth / 2;
        double dx = Math.Max(0, Math.Max(h.X - half - px, px - (h.X + half)));
        double dy = Math.Max(0, Math.Max(h.Y - half - py, py - (h.Y + half)));
        double dz = Math.Max(0, Math.Max(h.Z - pz, pz - (h.Z + h.Height)));
        return Math.Sqrt(dx * dx + dy * dy + dz * dz);
    }

    // Every change is an upsert, even "air" where the terrain is air: the other servers hear an upsert the
    // moment it is written, while a deleted record does not reach them. So a broken block is written as air.
    private void Set(int x, int y, int z, string kind, string by, string on, WorldUpdate update)
    {
        var cube = new WorldCube { key = Key(x, y, z), x = x, y = y, z = z, kind = kind, placed_by = by, placed_on = on };
        _overrides[cube.key] = cube;
        update.Changes.Add(new Change("upsert", cube));
    }

    /// <summary>Lets the column above (x, y, z) fall: every gravity block from the first unsupported one up.</summary>
    private void Settle(int x, int y, int z, WorldUpdate update)
    {
        var zz = BlockAt(x, y, z).Gravity ? z : z + 1;
        for (; zz < MaxZ && BlockAt(x, y, zz).Gravity; zz++)
        {
            var to = zz;
            while (to - 1 >= MinZ && !IsSolid(x, y, to - 1)) to--;
            if (to == zz) continue;

            var cube = _overrides.GetValueOrDefault(Key(x, y, zz));
            var kind = BlockAt(x, y, zz).Kind;
            Set(x, y, zz, "air", cube?.placed_by ?? "", cube?.placed_on ?? "", update);
            Set(x, y, to, kind, cube?.placed_by ?? "", cube?.placed_on ?? "", update);
            update.Falls.Add(new Fall(kind, x, y, zz, to));
        }
    }

    private static string Key(int x, int y, int z) => $"{x}:{y}:{z}";
}

/// <summary>What a player carries, per block kind.</summary>
public sealed class Inventory
{
    private readonly Dictionary<string, int> _stacks;

    public Inventory(Dictionary<string, int> stacks) => _stacks = stacks;

    public static Inventory Starting() => new(Spec.Placeable.ToDictionary(k => k, _ => Spec.StartingStack));

    public static Inventory Parse(string? stacks)
    {
        try
        {
            var parsed = string.IsNullOrEmpty(stacks) ? null : JsonSerializer.Deserialize<Dictionary<string, int>>(stacks);
            return parsed is null ? Starting() : new Inventory(parsed);
        }
        catch (JsonException) { return Starting(); }
    }

    public IReadOnlyDictionary<string, int> Stacks => _stacks;

    public int Count(string kind) => _stacks.GetValueOrDefault(kind);

    public bool Take(string kind)
    {
        if (Count(kind) <= 0) return false;
        _stacks[kind]--;
        return true;
    }

    /// <summary>Adds one item; a full stack (64) takes no more, as in Minecraft.</summary>
    public bool Give(string kind)
    {
        if (!Spec.Of(kind).Placeable || Count(kind) >= Spec.StackSize) return false;
        _stacks[kind] = Count(kind) + 1;
        return true;
    }

    public CubeInventory ToRecord(string playerId) => new()
    {
        player_id = playerId, cubes = _stacks.Values.Sum(), stacks = JsonSerializer.Serialize(_stacks),
    };
}
