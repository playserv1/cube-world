# GAS lab: the Gameplay Ability System across servers

Research prototype, 2026-10-07. Question from Crazybit after the Cube World demo: "how will it work with GAS?".
The full report (approaches, measurements, platform proposal) is the Claude Doc
"GAS across Cube World servers — research and prototype".

## What it is

`Source/CubeWorld/GasLab` is a small Lyra-style game inside the CubeWorld project: an `ACharacter` with its ability
system on the PlayerState (Mixed replication, Iris), and Cube World's creeper head rebuilt on GAS. A locally predicted
throw ability with a cooldown effect makes two specs, a blast (an execution capturing the thrower's BlastPower as a
snapshot and the target's Armor live) and a stacking Burning DoT. The creeper applies them where it blows.
Two to four Unreal dedicated servers each own one 24 m region of a 2 x 2 world.

Game code and the GameplayAbilities plugin know nothing of servers. Underneath them the border layer
(`GasLabBorder`, `GasLabAbilitySystemComponent`, `GasLabWire`) does the cross-server work:
- **Ghosts.** Players near a border are mirrored to the neighbour as ghosts with an ability system of their own.
- **Effects.** An effect applied to a ghost is sent to the owning server as bytes, and the owner builds an equal spec there.
- **Credit.** Damage and kills are credited back to the thrower's server.
- **Handoff.** A player who crosses takes the whole GAS state along: attribute bases, and active effects with their
  time left, stacks and next-tick phase.
- **Clock.** Every server and client uses one cluster clock, so GAS's effect time stamps mean the same everywhere.

The link between servers is either a direct TCP mesh (game port + 100) or `tools/S2SRelay`, a mock of a proposed
PlayServ server-to-server channel. Nothing touches the platform or dev.

## Run it

```powershell
dotnet build tools/S2SRelay -c Release                                    # once, for -Relay runs
dotnet tools/S2SRelay/bin/Release/net10.0/S2SRelay.dll 7900                # the mock relay
powershell -ExecutionPolicy Bypass -File unreal/CubeWorld/Scripts/RunGasLab.ps1 -Servers 4 -Scenario corner -Seconds 40
powershell -ExecutionPolicy Bypass -File unreal/CubeWorld/Scripts/RunGasLab.ps1 -Servers 2 -Scenario cross -Seconds 45
powershell -ExecutionPolicy Bypass -File unreal/CubeWorld/Scripts/RunGasLab.ps1 -Servers 2 -Scenario client -Client -Seconds 45
powershell -ExecutionPolicy Bypass -File unreal/CubeWorld/Scripts/RunGasLab.ps1 -Servers 4 -Scenario soak -Bots 8 -Seconds 75
python unreal/CubeWorld/Scripts/AnalyzeGasLab.py
```

Switches:
- `-Mode area`: the creeper carries its effects to each neighbour, instead of ghosts forwarding them.
- `-Relay ws://127.0.0.1:7900/s2s`: the link goes through the mock relay.
- `-NoClock`: each server keeps its own clock.
- `-TickRate 120`: the servers run at 120 Hz.
- `-Client -Window -ClientExtra "-gasshotevery=0.25"`: a visible client that takes screenshots into `Saved/Screenshots/GasLab`.

## What it showed (one machine, loopback, editor-run servers, bots and one headless client)

- **The corner case.** All 15 blasts hit players on four servers. The damage on the other servers matched the
  thrower's server to the logged 0.01, including the thrower's snapshotted BlastPower, and the credit came back.
- **One cross-server hop costs one to two server frames.** At 30 Hz the median was 21 ms over the direct link and
  65 ms over the relay; at 120 Hz, 16 and 31.5 ms. Today's data tables carry positions in 130–200 ms (dev).
- **Handoffs with the shared clock.** The restored timers added no error of their own (0.0 ms; every server read one
  machine's clock). Without the shared clock each effect ran long by its time on the wire (46 ms). Abilities halfway
  through a crossing are cancelled.
- **The soak.** 32 bots made 634 crossings in a minute, with 0 of 1,570 forwarded effects lost. This was loopback TCP
  with no faults injected; reconnects are untested.
- **The client.** The stock estimate of server time was off by a median of 0.9 s (editor-run client), and GAS never
  recomputes from it. `AGasLabGameState` fixes both.
