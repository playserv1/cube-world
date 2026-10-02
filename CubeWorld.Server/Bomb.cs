using PlayServ.Sdk.Data;

namespace CubeWorld.Server;

/// <summary>
/// A bomb. The drop function writes it "free" high above the world; it comes down under a parachute, a player picks
/// it up ("held"), throws it ("flying") and it explodes where it lands ("exploded"). When a sixth bomb is dropped
/// the oldest free one goes up in a puff of smoke ("fizzled"). A bomb only ever moves forward through these states,
/// so a server applies an update only when it moves the bomb on (<see cref="Bomb.Rank"/>).
/// </summary>
[EntityName("WorldBomb")]
public sealed class WorldBomb
{
    public string bomb_id { get; set; } = "";
    public string state { get; set; } = "";
    /// <summary>Who holds it, or who threw it.</summary>
    public string holder { get; set; } = "";
    /// <summary>Free: where it was dropped. Flying: where it left the hand. Exploded: where it went off.</summary>
    public double x { get; set; }
    public double y { get; set; }
    public double z { get; set; }
    /// <summary>Flying: the motion it was thrown with, blocks per tick. Empty on a bomb that was never thrown.</summary>
    public double? vx { get; set; }
    public double? vy { get; set; }
    public double? vz { get; set; }
    public long dropped_at { get; set; }
    /// <summary>When it entered its state, Unix milliseconds.</summary>
    public long at { get; set; }
}

public enum Flight { Flying, Exploded, Gone }

public static class Bomb
{
    public const string Free = "free", Held = "held", Flying = "flying", Exploded = "exploded", Fizzled = "fizzled";

    public static int Rank(string state) => state switch { Free => 0, Held => 1, Flying => 2, _ => 3 };

    public static bool Over(string state) => Rank(state) == 3;

    /// <summary>
    /// The seed a bomb's blast is worked out with, the same on every server (FNV-1a of its id; a string's
    /// GetHashCode differs from process to process).
    /// </summary>
    public static int BlastSeed(string bombId)
    {
        var hash = 2166136261u;
        foreach (var b in System.Text.Encoding.UTF8.GetBytes(bombId)) hash = (hash ^ b) * 16777619u;
        return (int)(hash & 0x7fffffff);
    }

    /// <summary>
    /// One tick under the parachute: down by <see cref="Spec.ParachuteSpeed"/> until it rests on a block. A bomb a
    /// block was put on climbs out on top. Returns the new height.
    /// </summary>
    public static double Descend(World world, double x, double y, double z)
    {
        int bx = (int)Math.Floor(x), by = (int)Math.Floor(y);
        if (world.IsSolid(bx, by, (int)Math.Floor(z))) return Math.Floor(z) + 1;
        var next = z - Spec.ParachuteSpeed;
        var below = (int)Math.Floor(next);
        return world.IsSolid(bx, by, below) || below < World.MinZ ? below + 1 : next;
    }

    /// <summary>
    /// A record heard live of a bomb this server does not follow, still in play though it was dropped long ago: a
    /// server that missed its end handed it out again (an Unreal server's ghost of a fizzled bomb). Every server hears a
    /// bomb from its drop on, so a live bomb is known; one loaded from the tables at start-up does not come this way.
    /// </summary>
    public static bool IsGhost(WorldBomb bomb, bool known, long now) =>
        !known && !Over(bomb.state) && now - bomb.dropped_at > FinishedBombs.KeepMs;

    /// <summary>
    /// How recently a bomb may have been dropped and still have no row in a read of the bomb table: its first rows may be
    /// written while the read runs, or pushed before they are stored. Older, a bomb with no row is long over (PSV-2977).
    /// </summary>
    public const long NoRowGraceMs = 10_000;

    /// <summary>How long a bomb in play must have no row in every read of the bomb table before it goes out of play.</summary>
    public const long RecheckMs = 5_000;

    /// <summary>
    /// The bombs a read of the bomb table takes out of play (PSV-2977): in play here, dropped more than
    /// <see cref="NoRowGraceMs"/> ago, and with no row in any read for <see cref="RecheckMs"/>. A bomb's rows go two
    /// minutes after it went off or fizzled (the drop function's sweep), so such a bomb is long over and this server
    /// missed its end. One read is not enough: a read of more than one page can skip a row that another write moved
    /// (PSV-3014). <paramref name="missingSince"/> holds when each bomb was first found with no row, and keeps that for
    /// the bombs this read did not take out. ACubeWorldGameMode::BombsGoneFromTable on the Unreal side.
    /// </summary>
    public static List<string> GoneFromTable(IReadOnlyDictionary<string, long> droppedAt, IReadOnlySet<string> inTable,
        Dictionary<string, long> missingSince, long now)
    {
        var gone = new List<string>();
        var stillMissing = new Dictionary<string, long>();
        // A read that found no row at all says nothing of any bomb: while a room is up, the table holds the bombs in play.
        if (inTable.Count > 0)
            foreach (var (id, dropped) in droppedAt)
            {
                if (inTable.Contains(id) || now - dropped <= NoRowGraceMs) continue;
                var known = missingSince.TryGetValue(id, out var since);
                if (known && now - since >= RecheckMs) gone.Add(id);
                else stillMissing[id] = known ? since : now;
            }
        missingSince.Clear();
        foreach (var (id, since) in stillMissing) missingSince[id] = since;
        return gone;
    }

    /// <summary>The bombs still in play over <paramref name="region"/>: when its room closes they go up in smoke.</summary>
    public static IEnumerable<WorldBomb> InRegion(IEnumerable<WorldBomb> bombs, int region) =>
        bombs.Where(b => !Over(b.state) && World.RegionOf(b.x, b.y) == region);

    /// <summary>Whether a player standing in <paramref name="p"/> is close enough to pick the bomb up.</summary>
    public static bool InPickupReach(Hitbox p, double x, double y, double z)
    {
        var reach = Spec.PlayerWidth / 2 + Spec.PickupReach;
        return Math.Abs(x - p.X) <= reach && Math.Abs(y - p.Y) <= reach
            && z >= p.Z - Spec.PickupReachUp && z <= p.Z + p.Height + Spec.PickupReachUp;
    }

    /// <summary>
    /// One tick of a thrown bomb, as a snowball flies: it moves by its motion, then the motion is multiplied by
    /// the drag and gravity pulls it down. It explodes at the last free point before a block or at the player it
    /// hits (its thrower only after a few ticks); off the edge of the world it is gone.
    /// </summary>
    public static Flight Fly(World world, double[] p, double[] v, IEnumerable<(string Id, Hitbox Box)> players, string owner, int age)
    {
        var length = Math.Sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        var steps = Math.Max(1, (int)Math.Ceiling(length / 0.1));
        for (var i = 1; i <= steps; i++)
        {
            double x = p[0] + v[0] / steps, y = p[1] + v[1] / steps, z = p[2] + v[2] / steps;
            if (x < 0 || x >= World.Width || y < 0 || y >= World.Depth || z < World.MinZ) return Flight.Gone;
            if (world.IsSolid((int)Math.Floor(x), (int)Math.Floor(y), (int)Math.Floor(z))) return Flight.Exploded;
            (p[0], p[1], p[2]) = (x, y, z);
            foreach (var (id, box) in players)
                if ((id != owner || age >= Spec.OwnerImmunityTicks) && Inside(box, x, y, z)) return Flight.Exploded;
        }
        for (var a = 0; a < 3; a++) v[a] *= Spec.ProjectileDrag;
        v[2] -= Spec.ProjectileGravity;
        return Flight.Flying;
    }

    private static bool Inside(Hitbox b, double x, double y, double z)
    {
        var half = Spec.PlayerWidth / 2;
        return Math.Abs(x - b.X) <= half && Math.Abs(y - b.Y) <= half && z >= b.Z && z <= b.Z + b.Height;
    }
}

/// <summary>
/// The bombs that went off or fizzled, kept for as long as the drop function keeps their rows: any later record of
/// one is stale (a server that missed the end) and must not bring it back into play.
/// </summary>
public sealed class FinishedBombs
{
    public const long KeepMs = 15 * 60_000;

    private readonly Dictionary<string, long> _over = new();

    public void Remember(string bombId, long at)
    {
        _over[bombId] = Math.Max(at, _over.GetValueOrDefault(bombId));
        if (_over.Count > 512) Forget(at);
    }

    public bool Has(string bombId, long now) => _over.TryGetValue(bombId, out var at) && now - at < KeepMs;

    private void Forget(long now)
    {
        foreach (var id in _over.Where(p => now - p.Value >= KeepMs).Select(p => p.Key).ToArray()) _over.Remove(id);
    }
}
