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
    /// <summary>When it was written (unix ms): the Unreal servers hear blocks written after the last one they saw.</summary>
    [System.Text.Json.Serialization.JsonNumberHandling(System.Text.Json.Serialization.JsonNumberHandling.AllowReadingFromString)]
    public double? at { get; set; }
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

    /// <summary>
    /// The second of two poses heard of one player shows them hurt: less health than a pose heard within the last 5 s
    /// (an older one may predate a stay on this very server, where they could have healed and been hurt again).
    /// </summary>
    public static bool WasHurt(WorldPresence? before, WorldPresence now) =>
        before is not null && now.health < before.health && now.health > 0 && now.seen_at - before.seen_at is >= 0 and < 5000;

    /// <summary>
    /// How long a server keeps the row of a player who left before it deletes it. One who crossed is written by the next
    /// server well within it; a row deleted at once left nobody in the table until then, so every server lost the player
    /// for up to a second and the next one put them at its spawn with full health (PSV-3018). CubeLeaveGraceMs on the
    /// Unreal side.
    /// </summary>
    public const int LeaveGraceMs = 2000;

    /// <summary>
    /// The newest pose heard of a player who left this server is another server's, written after this server's last
    /// one: they crossed, and that server holds them now. A pose another server wrote before they came here does not count.
    /// </summary>
    public static bool TakenOver(WorldPresence? heard, WorldPresence ours) =>
        heard is not null && heard.server != ours.server && heard.seen_at >= ours.seen_at;

    /// <summary>
    /// A presence row was deleted: it takes the player out unless the pose known of them came from another server than
    /// the one whose row went (a row left over from an older race, while the server that holds them goes on writing).
    /// ACubeWorldGameMode::DeleteTakesOut on the Unreal side.
    /// </summary>
    public static bool DeleteTakesOut(WorldPresence? known, WorldPresence deleted) =>
        known is not null && (string.IsNullOrEmpty(deleted.server) || known.server == deleted.server);

    /// <summary>How far outside a region the last server may have seen a player who walked in over its border, in blocks.</summary>
    public const double CrossingBand = 4;

    /// <summary>
    /// Where a player who joins this server stands. One another server saw in the last 5 s, alive, comes with the
    /// health they had. Seen in this server's region or within <see cref="CrossingBand"/> of it, they walked over the
    /// border: they stand where that server last saw them, moved inside the region. Seen farther away, they jumped here
    /// from the server list, and start at <paramref name="spawn"/>: a jump moves them, it does not heal them. A player
    /// nobody has seen in the last 5 s starts at the spawn, whole. A crossing pose keeps the time it was heard, so this
    /// server does not announce it again as new: a player who is walking is announced with their first move here, not a
    /// step behind. FCubeServerWorld's CubeCrossedInto on the Unreal side.
    /// </summary>
    public static WorldPresence Arriving(WorldPresence spawn, WorldPresence? heard, long now)
    {
        if (heard is null || now - heard.seen_at >= 5000 || heard.health <= 0) return spawn;
        // The spawn is the region's middle, so it names the region.
        var (x0, x1, y0, y1) = World.Bounds(World.RegionOf(spawn.x, spawn.y));
        var crossed = heard.x >= x0 - CrossingBand && heard.x <= x1 + CrossingBand && heard.y >= y0 - CrossingBand && heard.y <= y1 + CrossingBand;
        if (!crossed) return new()
        {
            player_id = spawn.player_id, name = spawn.name, server = spawn.server, color = spawn.color,
            x = spawn.x, y = spawn.y, z = spawn.z, health = heard.health,
        };
        const double half = Spec.PlayerWidth / 2;
        return new()
        {
            player_id = spawn.player_id, name = spawn.name, server = spawn.server, color = spawn.color,
            x = Math.Clamp(heard.x, x0 + half, x1 - half), y = Math.Clamp(heard.y, y0 + half, y1 - half), z = heard.z,
            yaw = heard.yaw, pitch = heard.pitch, health = heard.health,
            sneaking = heard.sneaking, sprinting = heard.sprinting, seen_at = heard.seen_at,
        };
    }
}

/// <summary>The fall a player is in, as their moves report it. FCubePlayerFall on the Unreal side.</summary>
public sealed class PlayerFall
{
    public bool Airborne { get; private set; }
    public double Peak { get; private set; }

    /// <summary>
    /// One move. In the air, the peak rises with it; on landing, the damage: ceil(peak - z - 3), never below 0, and the
    /// fall is over. <paramref name="saidPeak"/> is the client's own highest point since the ground, which carries a
    /// fall over a border: the part of it that happened on the old server counts too. It never makes a fall shorter
    /// than the moves themselves reached.
    /// </summary>
    public double Step(double z, bool onGround, double? saidPeak = null)
    {
        if (onGround)
        {
            var damage = Airborne ? Math.Max(0, Math.Ceiling(Peak - z - Spec.SafeFallDistance)) : 0;
            Airborne = false;
            return damage;
        }
        Peak = Airborne ? Math.Max(Peak, z) : z;
        if (saidPeak is { } said) Peak = Math.Max(Peak, Math.Min(said, World.MaxZ + 8));
        Airborne = true;
        return 0;
    }
}

public enum MoveVerdict { Accepted, Stale, Refused }

/// <summary>
/// How far a player's moves may take them (Spec.MoveSpeed). FCubeMoveCheck on the Unreal side. A move past the
/// allowance is refused, and the player is put back where the last accepted move left them: the server sends a
/// correction numbered <see cref="Seq"/>, and a client that took it says so in its moves, so the moves it sent before
/// it heard of it are dropped rather than refused again. A client that never numbers its moves cannot take a correction
/// (a Windows build from before 2026-10-02): refused for <see cref="UnnumberedGiveUpMs"/> on end, it is taken where it
/// says, so a player is never held in one place for good. Drop that once every client numbers its moves.
/// </summary>
public sealed class MoveCheck
{
    public const long UnnumberedGiveUpMs = 1000;

    private long _at, _refusedSince = -1;
    private double _allowance;
    private int _arrivedIn = -1;

    public double X { get; private set; }
    public double Y { get; private set; }
    public double Z { get; private set; }
    public int Seq { get; private set; }

    /// <summary>The server put the player here: they came back from the dead.</summary>
    public void Reset(double x, double y, double z, long now)
    {
        (X, Y, Z, _at, _allowance, _refusedSince, _arrivedIn) = (x, y, z, now, Spec.MoveBurst, -1, -1);
    }

    /// <summary>
    /// The player joined, or walked in over a border, and the server guessed they stand at (x, y, z): where the last
    /// server saw them, or the region's spawn when it saw them too long ago or too far off. A client that crossed plays on
    /// where it stands, which can be well past that guess, so the first move is taken as it comes when it is in this
    /// region or just past its border; only a first move from anywhere else is put back to the guess. Anchored on the
    /// guess, a player who crossed was snapped to a region's middle, and from there over and over between two rooms.
    /// </summary>
    public void Arrive(double x, double y, double z, int region, long now)
    {
        Reset(x, y, z, now);
        _arrivedIn = region;
    }

    /// <summary>A hit threw the player: they may fly further than they walk.</summary>
    public void Knocked(double strength) => _allowance += Math.Max(0, strength) * Spec.KnockbackReach;

    public MoveVerdict Check(double x, double y, double z, int? seq, long now)
    {
        if (seq is { } said && said < Seq) return MoveVerdict.Stale;
        if (_arrivedIn >= 0)
        {
            var near = World.Near(_arrivedIn, x, y, Spec.BorderSlack);
            _arrivedIn = -1;
            if (near)
            {
                Reset(x, y, z, now);
                return MoveVerdict.Accepted;
            }
            if (seq is null) _refusedSince = now;
            Seq++;
            return MoveVerdict.Refused;
        }

        // The allowance fills with time up to the burst; a knockback's extra stays until it is spent.
        _allowance = Math.Min(_allowance + Spec.MoveSpeed * Math.Max(0, now - _at) / 1000.0, Math.Max(_allowance, Spec.MoveBurst));
        _at = now;
        var distance = Math.Sqrt((x - X) * (x - X) + (y - Y) * (y - Y)) + Math.Max(0, z - Z);
        if (distance > _allowance + Spec.MoveSlack)
        {
            if (seq is null && _refusedSince >= 0 && now - _refusedSince >= UnnumberedGiveUpMs)
            {
                Reset(x, y, z, now);
                return MoveVerdict.Accepted;
            }
            if (_refusedSince < 0) _refusedSince = now;
            Seq++;
            return MoveVerdict.Refused;
        }
        _allowance = Math.Max(0, _allowance - distance);
        (X, Y, Z, _refusedSince) = (x, y, z, -1);
        return MoveVerdict.Accepted;
    }
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
    public double? at { get; set; }
}

[EntityName("WorldRegion")]
public sealed class WorldRegion
{
    public string region { get; set; } = "";
    public string server { get; set; } = "";
    public string color { get; set; } = "";
    public string room { get; set; } = "";
    /// <summary>The room type the room is registered under: the C# servers' and the Unreal servers' differ.</summary>
    public string slug { get; set; } = "";
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
    // Six regions, three across and two deep, like the six of a die: the upper row is the C# servers', the lower the
    // Unreal servers' (each prefers its own row and takes any free region when its row is full).
    public const int RegionSize = 24, Columns = 3, Rows = 2, Width = RegionSize * Columns, Depth = RegionSize * Rows, MinZ = -4, MaxZ = 64;
    public static readonly string[] RegionColors = ["red", "blue", "green", "yellow", "purple", "pink"];

    private readonly Dictionary<string, WorldCube> _overrides = new();

    public IEnumerable<WorldCube> Overrides => _overrides.Values;

    /// <summary>The region a spot belongs to: region r is column r % <see cref="Columns"/> of row r / <see cref="Columns"/>.</summary>
    public static int RegionOf(double x, double y) => (int)Math.Floor(y / RegionSize) * Columns + (int)Math.Floor(x / RegionSize);

    /// <summary>The blocks of <paramref name="region"/>: x in [X0, X1), y in [Y0, Y1).</summary>
    public static (int X0, int X1, int Y0, int Y1) Bounds(int region) =>
        (region % Columns * RegionSize, (region % Columns + 1) * RegionSize, region / Columns * RegionSize, (region / Columns + 1) * RegionSize);

    /// <summary>
    /// A spot inside <paramref name="region"/> or within <paramref name="slack"/> blocks of it. A server edits the world
    /// only for players who stand in its region: one standing anywhere else edits through that region's server (a
    /// player who walks over a border plays on with the old server for the moment the crossing takes, hence the slack).
    /// </summary>
    public static bool Near(int region, double x, double y, double slack)
    {
        if (region < 0) return false;
        var (x0, x1, y0, y1) = Bounds(region);
        return x >= x0 - slack && x <= x1 + slack && y >= y0 - slack && y <= y1 + slack;
    }

    /// <summary>
    /// Whether the server of <paramref name="region"/> changes the world for a player standing at x, y, who has been
    /// outside its region for <paramref name="outsideMs"/>: digs, placements, bombs picked up and thrown. In its region,
    /// yes. Just past its border, in a region another live server holds (<paramref name="heldElsewhere"/>), only for the
    /// moment a crossing takes. Anywhere else no: a player who stays with the old server because the next room did not
    /// let them in, or who walked into a region no live server holds, can only walk there.
    /// </summary>
    public static bool Serves(int region, double x, double y, long outsideMs, IReadOnlyCollection<int> heldElsewhere) =>
        Near(region, x, y, 0)
        || (Near(region, x, y, Spec.BorderSlack) && outsideMs <= Spec.CrossingMs && heldElsewhere.Contains(RegionOf(x, y)));

    /// <summary>
    /// Whether the server of <paramref name="region"/> may change the block at x, y: one of its own region, or of a region
    /// another live server holds. A region no live server holds keeps its blocks until a server claims it.
    /// </summary>
    public static bool ServesBlock(int region, int x, int y, IReadOnlyCollection<int> heldElsewhere)
    {
        var there = RegionOf(x + 0.5, y + 0.5);
        return (region >= 0 && there == region) || heldElsewhere.Contains(there);
    }

    /// <summary>Where a region's players spawn: its middle.</summary>
    public static (double X, double Y) Centre(int region) => ((region % Columns + 0.5) * RegionSize, (region / Columns + 0.5) * RegionSize);

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
        cube.kind = Spec.Canonical(cube.kind);
        if (!Inside(cube.x, cube.y, cube.z) || _overrides.TryGetValue(cube.key, out var known) && known.kind == cube.kind) return false;
        _overrides[cube.key] = cube;
        return true;
    }

    /// <summary>
    /// A row read back from the table: applied only when it is newer than what this server holds, so a read that
    /// crossed one of this server's own writes does not undo it. Returns whether it changed anything here.
    /// </summary>
    public bool Reconcile(WorldCube cube, string self, long now)
    {
        if (cube.at is not { } at) return false;
        if (_overrides.TryGetValue(cube.key, out var known))
        {
            if (known.at is { } knownAt && knownAt >= at) return false;
            // This server's own write of the last minute may not be in the table yet, and a writer whose clock runs
            // ahead could have stamped the older row later: the live updates settle such a block, the read-back does not.
            if (known.placed_on == self && known.at is { } ownAt && now - ownAt < 60_000) return false;
        }
        return Apply("upsert", cube);
    }

    public void Load(IEnumerable<WorldCube> cubes)
    {
        foreach (var cube in cubes)
        {
            cube.kind = Spec.Canonical(cube.kind);
            _overrides[cube.key] = cube;
        }
    }

    /// <summary>
    /// An explosion as Minecraft's: rays go out from the centre towards every point of a 16 × 16 × 16 cube's
    /// surface, each with an intensity of power × (0.7 to 1.3). Every 0.3 blocks a ray loses 0.225 and, in a
    /// block, (blast resistance + 0.3) × 0.3; a block the ray still has intensity for is destroyed. Nothing drops.
    /// With <paramref name="region"/> only that region's blocks are broken, though the rays are worked out
    /// through the whole world: each server breaks its own share of a blast, and with the same
    /// <paramref name="random"/> seed the shares add up to the one crater.
    /// </summary>
    public WorldUpdate Explode(double cx, double cy, double cz, double power, Random random, string by, string on, int? region = null)
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

        if (region is { } only) destroyed.RemoveWhere(b => RegionOf(b.x, b.y) != only);

        var update = new WorldUpdate();
        foreach (var (x, y, z) in destroyed.OrderBy(b => b.z)) Set(x, y, z, "air", by, on, update);
        // Each broken block of a column settles what stands on it: a blast can break a column in more than one place.
        foreach (var (x, y, z) in destroyed.OrderBy(b => b.z)) Settle(x, y, z, update);
        return update;
    }

    /// <summary>
    /// What an explosion does to a player, as Minecraft works it out but within Spec.BlastReach, not twice the
    /// power: impact is (1 − distance / reach) × the share of the hitbox the centre can see; damage is
    /// ⌊(impact² + impact) / 2 × 7 × 2 × power + 1⌋ and the player is thrown away from the centre with the impact.
    /// </summary>
    public (double Damage, double Nx, double Ny, double Impact)? Blast(double cx, double cy, double cz, double power, Hitbox p, double eye)
    {
        double fx = p.X - cx, fy = p.Y - cy, fz = p.Z - cz;
        var distance = Math.Sqrt(fx * fx + fy * fy + fz * fz) / Spec.BlastReach;
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
        var cube = new WorldCube { key = Key(x, y, z), x = x, y = y, z = z, kind = kind, placed_by = by, placed_on = on, at = DateTimeOffset.UtcNow.ToUnixTimeMilliseconds() };
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
            return parsed is null ? Starting() : new Inventory(Canonical(parsed));
        }
        catch (JsonException) { return Starting(); }
    }

    /// <summary>
    /// The stacks under the kinds' own names. A row an Unreal server wrote says "Stone", and the refill function then
    /// added a "stone" beside it: the two are one kind, and the larger count is the player's (the refill's top-up only
    /// started from nothing because it did not know the other spelling).
    /// </summary>
    public static Dictionary<string, int> Canonical(IReadOnlyDictionary<string, int> stacks)
    {
        var merged = new Dictionary<string, int>();
        foreach (var (kind, count) in stacks)
        {
            var name = Spec.Canonical(kind);
            merged[name] = Math.Max(merged.GetValueOrDefault(name), count);
        }
        return merged;
    }

    public IReadOnlyDictionary<string, int> Stacks => _stacks;

    public int Count(string kind) => _stacks.GetValueOrDefault(Spec.Canonical(kind));

    public bool Take(string kind)
    {
        kind = Spec.Canonical(kind);
        if (Count(kind) <= 0) return false;
        _stacks[kind]--;
        return true;
    }

    /// <summary>Adds one item; a full stack (64) takes no more, as in Minecraft.</summary>
    public bool Give(string kind)
    {
        kind = Spec.Canonical(kind);
        if (!Spec.Of(kind).Placeable || Count(kind) >= Spec.StackSize) return false;
        _stacks[kind] = Count(kind) + 1;
        return true;
    }

    public CubeInventory ToRecord(string playerId) => new()
    {
        player_id = playerId, cubes = _stacks.Values.Sum(), stacks = JsonSerializer.Serialize(_stacks),
    };

    /// <summary>Holds these stacks from now on (a merge of another writer's row).</summary>
    public void Set(IReadOnlyDictionary<string, int> stacks)
    {
        _stacks.Clear();
        foreach (var (kind, count) in stacks) _stacks[kind] = count;
    }
}

/// <summary>
/// One player's inventory row as this server knows it, for merging the other writers' rows into the inventory it holds:
/// the old server's last write after a crossing, the refill function's top-up. A row heard is either this server's own
/// write coming back (one of the rows it wrote and has not heard yet) or someone else's: then what that writer changed
/// since the row this server last knew is added to what the player did here, kind by kind. FCubeInventorySync on the
/// Unreal side.
/// </summary>
public sealed class InventorySync(IReadOnlyDictionary<string, int> read)
{
    /// <summary>
    /// A write of ours not heard back after this long is taken to be in the row: should the uplink not send a server its
    /// own writes, another writer's change would otherwise be counted from a row older than them.
    /// </summary>
    public const long EchoMs = 3000;

    private readonly List<(Dictionary<string, int> Stacks, long At)> _written = [];

    /// <summary>The row as this server last knew it: read at the join, or heard since.</summary>
    public IReadOnlyDictionary<string, int> Base { get; private set; } = new Dictionary<string, int>(read);

    public void Wrote(IReadOnlyDictionary<string, int> stacks, long now)
    {
        _written.Add((new Dictionary<string, int>(stacks), now));
        if (_written.Count > 32) _written.RemoveAt(0);
    }

    /// <summary>A row was heard. Null when it is this server's own write (nothing changes); else the inventory to hold now.</summary>
    public Dictionary<string, int>? Heard(IReadOnlyDictionary<string, int> ours, IReadOnlyDictionary<string, int> theirs, long now)
    {
        // The platform keeps one write of a row at a time and sends the latest: a later write of ours heard means the
        // earlier ones are behind us too.
        var own = _written.FindIndex(w => Same(w.Stacks, theirs));
        if (own >= 0)
        {
            _written.RemoveRange(0, own + 1);
            Base = new Dictionary<string, int>(theirs);
            return null;
        }
        var landed = _written.FindLastIndex(w => now - w.At >= EchoMs);
        if (landed >= 0)
        {
            Base = _written[landed].Stacks;
            _written.RemoveRange(0, landed + 1);
        }
        var merged = Merge(ours, Base, theirs);
        Base = new Dictionary<string, int>(theirs);
        return merged;
    }

    /// <summary>Ours plus what theirs changed since the base, each kind kept within 0 and a stack.</summary>
    public static Dictionary<string, int> Merge(IReadOnlyDictionary<string, int> ours, IReadOnlyDictionary<string, int> @base, IReadOnlyDictionary<string, int> theirs) =>
        ours.Keys.Union(@base.Keys).Union(theirs.Keys).ToDictionary(kind => kind,
            kind => Math.Clamp(ours.GetValueOrDefault(kind) + theirs.GetValueOrDefault(kind) - @base.GetValueOrDefault(kind), 0, Spec.StackSize));

    public static bool Same(IReadOnlyDictionary<string, int> a, IReadOnlyDictionary<string, int> b) =>
        a.Keys.Union(b.Keys).All(kind => a.GetValueOrDefault(kind) == b.GetValueOrDefault(kind));
}
