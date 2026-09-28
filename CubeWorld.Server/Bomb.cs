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
    /// <summary>Flying: the motion it was thrown with, blocks per tick.</summary>
    public double vx { get; set; }
    public double vy { get; set; }
    public double vz { get; set; }
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

    /// <summary>The free bombs beyond the limit, oldest first: they go up in smoke.</summary>
    public static IEnumerable<WorldBomb> Surplus(IEnumerable<WorldBomb> bombs, int max) =>
        bombs.Where(b => b.state == Free).OrderByDescending(b => b.dropped_at).ThenByDescending(b => b.bomb_id).Skip(max);

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
