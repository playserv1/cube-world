using PlayServ.Sdk;

namespace CubeWorld.Server;

/// <summary>
/// The bombs: they come down under a parachute, a player picks one up and throws it, it blows a crater.
/// Every server brings every free bomb down, but only the server of the region a bomb is over lets a player pick
/// it up, so two servers never hand out one bomb. A thrown bomb is flown by its thrower's server alone.
/// </summary>
public sealed partial class CubeWorldServer
{
    /// <summary>The bombs the tables hold. They are an extra: a world whose bombs cannot be read still opens, without them.</summary>
    private async Task LoadBombs()
    {
        try
        {
            foreach (var bomb in await LoadBombsAsync())
            {
                lock (_world) OnBomb(bomb, owned: false);
            }
        }
        catch (Exception e) { _ = Platform.Log($"bombs not loaded, the world opens without them: {e.Message}"); }
    }

    /// <summary>Every tick: free bombs come down and are picked up, thrown ones fly and go off.</summary>
    private void MoveBombs()
    {
        lock (_world)
        {
            foreach (var live in _bombs.Values.ToArray())
            {
                if (live.Record.state == Bomb.Free) Parachute(live);
                else if (live.Record.state == Bomb.Flying && live.Owned) Fly(live);
            }
        }
    }

    private void Parachute(LiveBomb live)
    {
        var bomb = live.Record;
        live.Z = Bomb.Descend(_world, bomb.x, bomb.y, live.Z);
        if (World.RegionOf(bomb.x) != _region) return;

        var taker = _players.Values.FirstOrDefault(p => !p.Dead && p.Bomb is null
                                                        && Bomb.InPickupReach(HitboxOf(p.Pose), bomb.x, bomb.y, live.Z));
        if (taker is not null)
            Share(Next(bomb, Bomb.Held, taker.Pose.player_id, bomb.x, bomb.y, live.Z), owned: false);
    }

    private void Fly(LiveBomb live)
    {
        var bomb = live.Record;
        var flight = live.Age++ >= Spec.BombFlightTicks ? Flight.Exploded
            : Bomb.Fly(_world, live.P, live.V, Targets(), bomb.holder, live.Age);
        if (flight == Flight.Exploded) Explode(bomb, live.P);
        else if (flight == Flight.Gone) Share(Next(bomb, Bomb.Fizzled, bomb.holder, live.P[0], live.P[1], live.P[2]), owned: false);
    }

    private void Throw(Player player, Command command)
    {
        if (player.Dead || player.Bomb is not { } id) return;
        player.Bomb = null;
        if (!_bombs.TryGetValue(id, out var live) || live.Record.state != Bomb.Held || live.Record.holder != player.Pose.player_id) return;

        double dx = command.x, dy = command.y, dz = command.z;
        var length = Math.Sqrt(dx * dx + dy * dy + dz * dz);
        if (length < 1e-6)
        {
            var (yaw, pitch) = (player.Pose.yaw, player.Pose.pitch);
            (dx, dy, dz, length) = (-Math.Sin(yaw) * Math.Cos(pitch), Math.Cos(yaw) * Math.Cos(pitch), -Math.Sin(pitch), 1);
        }
        var (ex, ey, ez) = Eye(player);
        var thrown = Next(live.Record, Bomb.Flying, player.Pose.player_id, ex, ey, ez);
        (thrown.vx, thrown.vy, thrown.vz) = (dx / length * Spec.ThrowSpeed, dy / length * Spec.ThrowSpeed, dz / length * Spec.ThrowSpeed);
        Share(thrown, owned: true);
    }

    /// <summary>
    /// Players here are hurt, players elsewhere get a WorldHit, both judged against the world as it stood before the
    /// blast, as Minecraft does. Then the bomb is recorded as exploded, and <see cref="OnBomb"/> breaks this region's
    /// blocks; every other server breaks its own when it hears the record. The centre is rounded to a thousandth so
    /// the record carries exactly the point every server works the blast out from.
    /// </summary>
    private void Explode(WorldBomb bomb, double[] at)
    {
        var (cx, cy, cz) = (Math.Round(at[0], 3), Math.Round(at[1], 3), Math.Round(at[2], 3));

        foreach (var player in _players.Values.Where(p => !p.Dead))
            if (_world.Blast(cx, cy, cz, Spec.BombPower, HitboxOf(player.Pose), EyeHeightOf(player.Pose)) is { } blast)
                Hurt(player, blast.Damage, (blast.Nx, blast.Ny), blast.Impact, bomb.holder);

        foreach (var pose in OthersAlive())
            if (_world.Blast(cx, cy, cz, Spec.BombPower, HitboxOf(pose), EyeHeightOf(pose)) is { } blast)
                HitElsewhere($"{bomb.bomb_id}:{pose.player_id}", pose.player_id, bomb.holder, blast.Damage, (blast.Nx, blast.Ny), blast.Impact);

        Share(Next(bomb, Bomb.Exploded, bomb.holder, cx, cy, cz), owned: false);
    }

    /// <summary>
    /// A bomb went off, here or on another server: this server breaks the blocks of its own region and no others.
    /// A region whose server is not up when the bomb goes off keeps its blocks.
    /// </summary>
    private void Crater(WorldBomb bomb)
    {
        if (_region < 0) return;
        Publish(_world.Explode(bomb.x, bomb.y, bomb.z, Spec.BombPower, new Random(Bomb.BlastSeed(bomb.bomb_id)),
            bomb.holder, _server, _region));
    }

    /// <summary>The bombs over this region go up in smoke: its room was closed.</summary>
    private void FizzleBombsOverRegion()
    {
        lock (_world)
            foreach (var bomb in Bomb.InRegion(_bombs.Values.Select(b => b.Record), _region).ToArray())
                Share(Next(bomb, Bomb.Fizzled, bomb.holder, bomb.x, bomb.y, _bombs[bomb.bomb_id].Z), owned: false);
    }

    private static WorldBomb Next(WorldBomb bomb, string state, string holder, double x, double y, double z) => new()
    {
        bomb_id = bomb.bomb_id, state = state, holder = holder, x = x, y = y, z = z, dropped_at = bomb.dropped_at, at = Now,
    };

    private void Share(WorldBomb bomb, bool owned)
    {
        Platform.RuntimeData.Write(Uplink, "WorldBomb", bomb.bomb_id, bomb);
        OnBomb(bomb, owned);
    }

    private void HearBomb(WorldBomb bomb)
    {
        lock (_world) OnBomb(bomb, owned: false);
    }

    /// <summary>A bomb moved on, here or on another server. Anything that does not move it forward is an echo or stale.</summary>
    private void OnBomb(WorldBomb bomb, bool owned)
    {
        var known = _bombs.GetValueOrDefault(bomb.bomb_id);
        if (known is not null && Bomb.Rank(bomb.state) <= Bomb.Rank(known.Record.state)) return;
        if (known is null && Bomb.Over(bomb.state) && Now - bomb.at > 5000) return;

        if (Bomb.Over(bomb.state)) _bombs.Remove(bomb.bomb_id);
        else _bombs[bomb.bomb_id] = LiveBomb.Of(bomb, _world, owned, Now);
        if (bomb.state == Bomb.Exploded) Crater(bomb);

        foreach (var player in _players.Values)
            if (player.Bomb == bomb.bomb_id && (bomb.state != Bomb.Held || bomb.holder != player.Pose.player_id)) player.Bomb = null;
        if (bomb.state == Bomb.Held && _players.TryGetValue(bomb.holder, out var holder)) holder.Bomb = bomb.bomb_id;

        Broadcast(BombFrame(bomb, _bombs.GetValueOrDefault(bomb.bomb_id)));
    }

    private static object BombFrame(WorldBomb bomb, LiveBomb? live) => new { type = "bomb", bomb, age = Now - bomb.at, z = live?.Z ?? bomb.z };

    private IEnumerable<(string, Hitbox)> Targets() => EveryoneAlive().Select(p => (p.player_id, HitboxOf(p)));

    private static async Task<List<WorldBomb>> LoadBombsAsync()
    {
        var bombs = (await ReadAll(Platform.Table<WorldBomb>().Query())).Select(r => r.Fields!);
        // A bomb can have several rows (the drop function's and the servers'): the one furthest on is the bomb.
        return bombs.GroupBy(b => b.bomb_id)
            .Select(g => g.OrderByDescending(b => Bomb.Rank(b.state)).ThenByDescending(b => b.at).First())
            .Where(b => !Bomb.Over(b.state)).ToList();
    }

    /// <summary>A bomb as this server follows it: the height of a free one, the path of one it threw.</summary>
    private sealed class LiveBomb
    {
        public required WorldBomb Record { get; init; }
        public double Z { get; set; }
        public double[] P { get; init; } = [];
        public double[] V { get; init; } = [];
        public bool Owned { get; init; }
        public int Age { get; set; }

        /// <summary>A free bomb heard late is brought down as far as it has come since it was dropped.</summary>
        public static LiveBomb Of(WorldBomb bomb, World world, bool owned, long now)
        {
            var z = bomb.z;
            if (bomb.state == Bomb.Free)
                for (long t = 0, ticks = Math.Clamp((now - bomb.at) / (1000 / Spec.TicksPerSecond), 0, 2000); t < ticks; t++)
                    z = Bomb.Descend(world, bomb.x, bomb.y, z);
            return new LiveBomb
            {
                Record = bomb, Z = z, Owned = owned && bomb.state == Bomb.Flying,
                P = [bomb.x, bomb.y, bomb.z], V = [bomb.vx, bomb.vy, bomb.vz],
            };
        }
    }
}
