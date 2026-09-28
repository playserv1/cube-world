namespace CubeWorld.Server;

/// <summary>One block kind and the properties Minecraft gives it (SPEC.md names the wiki page for each number).</summary>
public sealed record Block(string Kind, double Hardness, bool NeedsTool, bool Transparent, bool Gravity, string? Drop, double BlastResistance)
{
    public bool Solid => Kind != "air";
    public bool Breakable => Hardness >= 0;
    public bool Placeable => Kind is not ("air" or "bedrock");

    /// <summary>
    /// Ticks to break by hand. Damage per tick is 1/(hardness·30) when no tool is required and 1/(hardness·100)
    /// when the block needs a tool the player is not holding; the block breaks when the damage reaches 1.
    /// </summary>
    public int BreakTicks => Breakable ? (int)Math.Ceiling(Hardness * (NeedsTool ? 100 : 30) - 1e-9) : int.MaxValue;
}

/// <summary>The canonical numbers, all from the Minecraft Wiki. Mirrored in web/spec.js.</summary>
public static class Spec
{
    public const int TicksPerSecond = 20;

    // Reach: 4.5 blocks for blocks, 3 for entities; the server allows a little extra for latency.
    public const double BlockReach = 4.5, EntityReach = 3.0, ReachTolerance = 1.0;

    // Player hitbox 0.6 wide, 1.8 tall (1.5 sneaking); eyes at 1.62 (1.27 sneaking).
    public const double PlayerWidth = 0.6, PlayerHeight = 1.8, SneakHeight = 1.5, EyeHeight = 1.62, SneakEyeHeight = 1.27;

    // Health 20; 10 ticks of invulnerability after a hit; natural regeneration of 1 every 80 ticks.
    public const double MaxHealth = 20;
    public const int InvulnerabilityTicks = 10, RegenIntervalTicks = 80;

    // An empty hand deals 1 damage, recharges in 5 ticks (attack speed 4) and knocks back 0.4; sprinting adds 0.5.
    public const double FistDamage = 1.0, Knockback = 0.4, SprintKnockback = 0.5;
    public const int FistChargeTicks = 5;

    // Fall damage: 1 per block fallen beyond the third.
    public const double SafeFallDistance = 3.0;

    public const int StackSize = 64, StartingStack = 64;

    // A bomb flies as Minecraft's thrown projectiles do: each tick it moves, its motion is multiplied by 0.99 and
    // gravity takes 0.05, a thrown potion's. It leaves the hand at 1 block a tick (a snowball's 1.5 would cross the
    // whole world), so it lands some 15 to 20 blocks away. It explodes where it hits, with a creeper's power of 3.
    public const double ThrowSpeed = 1.0, ProjectileDrag = 0.99, ProjectileGravity = 0.05, BombPower = 3;
    public const int BombFlightTicks = 200, OwnerImmunityTicks = 4;

    // A dropped bomb comes down under a parachute at 0.1 blocks a tick (2 m/s) from 32 blocks up. A player
    // picks it up as Minecraft players pick up items: the bomb within the hitbox grown by 1 sideways, 0.5 up and down.
    public const double ParachuteSpeed = 0.1, DropHeight = 32, PickupReach = 1.0, PickupReachUp = 0.5;

    // At most five bombs lie free; the drop function keeps to it and every server holds it too.
    public const int MaxFreeBombs = 5;

    public static readonly Block[] Blocks =
    [
        new("air", 0, false, true, false, null, 0),
        new("bedrock", -1, true, false, false, null, 3_600_000),
        new("grass", 0.6, false, false, false, "dirt", 0.6),
        new("dirt", 0.5, false, false, false, "dirt", 0.5),
        new("sand", 0.5, false, false, true, "sand", 0.5),
        new("stone", 1.5, true, false, false, null, 6),
        new("wood", 2.0, false, false, false, "wood", 2),
        new("brick", 2.0, true, false, false, null, 6),
        new("glass", 0.3, false, true, false, null, 0.3),
        new("gold", 3.0, true, false, false, null, 6),
        new("leaves", 0.2, false, true, false, null, 0.2),
    ];

    public static readonly Dictionary<string, Block> ByKind = Blocks.ToDictionary(b => b.Kind);

    /// <summary>Hotbar order.</summary>
    public static readonly string[] Placeable = Blocks.Where(b => b.Placeable).Select(b => b.Kind).ToArray();

    public static Block Of(string? kind) => kind is not null && ByKind.TryGetValue(kind, out var block) ? block : ByKind["air"];

    /// <summary>Superflat "Classic Flat": one bedrock, two dirt, one grass block. The player stands at z = 0.</summary>
    public static readonly (int z, string kind)[] Layers = [(-4, "bedrock"), (-3, "dirt"), (-2, "dirt"), (-1, "grass")];

    /// <summary>Where the oaks stand: four per region, clear of the spawn at the region's centre. Mirrored in web/voxels.js.</summary>
    public static readonly (int x, int y)[] Trees =
        Enumerable.Range(0, 3).SelectMany(r => new[] { (r * 24 + 4, 5), (r * 24 + 18, 4), (r * 24 + 6, 18), (r * 24 + 19, 17) }).ToArray();

    /// <summary>
    /// An oak: a trunk of five logs, two 5 × 5 layers of leaves without their corners around the top two logs,
    /// a 3 × 3 layer above the trunk and a cross on top. Keyed "x:y:z", z from 0 (the ground).
    /// </summary>
    public static readonly Dictionary<string, string> TreeBlocks = BuildTrees();

    private static Dictionary<string, string> BuildTrees()
    {
        var blocks = new Dictionary<string, string>();
        foreach (var (tx, ty) in Trees)
        {
            for (var dz = 0; dz < 5; dz++) blocks[$"{tx}:{ty}:{dz}"] = "wood";
            for (var dx = -2; dx <= 2; dx++)
                for (var dy = -2; dy <= 2; dy++)
                {
                    var corner = Math.Abs(dx) == 2 && Math.Abs(dy) == 2;
                    var trunk = dx == 0 && dy == 0;
                    for (var dz = 3; dz <= 4; dz++) if (!corner && !trunk) blocks.TryAdd($"{tx + dx}:{ty + dy}:{dz}", "leaves");
                    if (Math.Abs(dx) <= 1 && Math.Abs(dy) <= 1) blocks.TryAdd($"{tx + dx}:{ty + dy}:5", "leaves");
                    if (Math.Abs(dx) + Math.Abs(dy) <= 1) blocks.TryAdd($"{tx + dx}:{ty + dy}:6", "leaves");
                }
        }
        return blocks;
    }
}
