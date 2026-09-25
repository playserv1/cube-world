namespace CubeWorld.Server;

/// <summary>One block kind and the properties Minecraft gives it (SPEC.md names the wiki page for each number).</summary>
public sealed record Block(string Kind, double Hardness, bool NeedsTool, bool Transparent, bool Gravity, string? Drop)
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

    public static readonly Block[] Blocks =
    [
        new("air", 0, false, true, false, null),
        new("bedrock", -1, true, false, false, null),
        new("grass", 0.6, false, false, false, "dirt"),
        new("dirt", 0.5, false, false, false, "dirt"),
        new("sand", 0.5, false, false, true, "sand"),
        new("stone", 1.5, true, false, false, null),
        new("wood", 2.0, false, false, false, "wood"),
        new("brick", 2.0, true, false, false, null),
        new("glass", 0.3, false, true, false, null),
        new("gold", 3.0, true, false, false, null),
    ];

    public static readonly Dictionary<string, Block> ByKind = Blocks.ToDictionary(b => b.Kind);

    /// <summary>Hotbar order.</summary>
    public static readonly string[] Placeable = Blocks.Where(b => b.Placeable).Select(b => b.Kind).ToArray();

    public static Block Of(string? kind) => kind is not null && ByKind.TryGetValue(kind, out var block) ? block : ByKind["air"];

    /// <summary>Superflat "Classic Flat": one bedrock, two dirt, one grass block. The player stands at z = 0.</summary>
    public static readonly (int z, string kind)[] Layers = [(-4, "bedrock"), (-3, "dirt"), (-2, "dirt"), (-1, "grass")];
}
