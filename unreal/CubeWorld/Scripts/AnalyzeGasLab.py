"""Reads the GAS lab's server logs (Saved/Logs/gaslab-<tag>-<region>.log, written by Scripts/RunGasLab.ps1) and prints
what the research measures, per run:

  corner  whether a blast did the same damage on every server (the thrower's snapshot travelled), how long after the
          blast each server applied it, how long the credit took to come back;
  cross   each handoff: time on the wire, the error of every restored effect's end and next tick, and the burning
          ticks of the player who crossed, one second apart or not;
  soak    handoffs, effects that had to be forwarded again, anything lost;
  all     the link's round trip (probes) and the bytes a server sends a second.

  python Scripts/AnalyzeGasLab.py [tag ...]        # default: every tag found
  python Scripts/AnalyzeGasLab.py --json out.json  # also writes the numbers
"""
import glob
import json
import os
import re
import statistics
import sys

LOGS = os.path.join(os.path.dirname(__file__), "..", "Saved", "Logs")
LINE = re.compile(r"GASLAB (\S+)(.*)")
FIELD = re.compile(r"(\w+)=(\[[^\]]*\]|\S+)")


def num(text):
    try:
        return float(text.rstrip("ms"))
    except ValueError:
        return None


def parse(path, region):
    events = []
    with open(path, encoding="utf-8", errors="replace") as f:
        for raw in f:
            m = LINE.search(raw)
            if not m:
                continue
            fields = dict(FIELD.findall(m.group(2)))
            fields["_event"] = m.group(1)
            fields["_server"] = region
            events.append(fields)
    return events


def pct(values, p):
    if not values:
        return None
    values = sorted(values)
    k = (len(values) - 1) * p / 100.0
    lo, hi = int(k), min(int(k) + 1, len(values) - 1)
    return values[lo] + (values[hi] - values[lo]) * (k - lo)


def dist(values):
    values = [v for v in values if v is not None]
    if not values:
        return None
    return {"n": len(values), "p50": round(pct(values, 50), 1), "p95": round(pct(values, 95), 1), "max": round(max(values), 1)}


def floats(text):
    text = text.strip("[]")
    return [float(x) for x in text.split(",") if x]


def analyze(tag):
    events = []
    for path in sorted(glob.glob(os.path.join(LOGS, f"gaslab-{tag}-[0-9].log"))):
        region = int(path.rsplit("-", 1)[1].split(".")[0])
        events += parse(path, region)
    if not events:
        return None
    out = {"tag": tag, "servers": len({e["_server"] for e in events})}
    of = lambda name: [e for e in events if e["_event"] == name]

    probes = [num(e["rtt"]) for e in of("probe")]
    out["probe_rtt_ms"] = dist(probes)
    stats = [num(e["out"].replace("B/s", "")) for e in of("stats") if e.get("players", "0") != "0"]
    out["link_out_bytes_per_s"] = dist(stats)

    explodes = sorted(of("explode"), key=lambda e: num(e["at"]))
    hits = of("hit")
    blasts = [h for h in hits if h.get("effect") == "Blast"]
    if explodes and blasts:
        rows = []
        for x in explodes:
            t = num(x["at"])
            owner = int(x["region"])
            group = [h for h in blasts if 0 <= num(h["at"]) - t < 1.0]
            by_kind = {}
            for h in group:
                kind = re.sub(r"\d+$", "", h["target"])
                by_kind.setdefault(kind, []).append(h)
            spread = 0.0
            for kind, hs in by_kind.items():
                local = [num(h["dmg"]) for h in hs if h["_server"] == owner and num(h["hp"]) > 0]
                if local:
                    spread = max([spread] + [abs(num(h["dmg"]) - local[0]) for h in hs if num(h["hp"]) > 0])
            rows.append({"creeper": x["creeper"], "mode": x.get("mode"), "servers_hit": len({h["_server"] for h in group}),
                         "targets_hit": len(group), "max_damage_difference": round(spread, 3)})
        out["blasts"] = rows
        remote = [num(h["lag"]) for x in explodes for h in blasts
                  if 0 <= num(h["at"]) - num(x["at"]) < 1.0 and h["_server"] != int(x["region"])]
        local = [num(h["lag"]) for x in explodes for h in blasts
                 if 0 <= num(h["at"]) - num(x["at"]) < 1.0 and h["_server"] == int(x["region"])]
        out["blast_lag_local_ms"] = dist(local)
        out["blast_lag_remote_ms"] = dist(remote)
        out["credit_lag_ms"] = dist([num(e["lag"]) for e in of("credit")])

    restores = of("handoff-restore")
    if restores:
        out["handoffs"] = [{"id": e["id"], "transit_ms": num(e["transit"]), "effects": e["effects"],
                            "end_error_ms": floats(e["enderr"]), "tick_phase_error_ms": floats(e["phaseerr"]),
                            "stacks": int(e["stacks"]), "burn_left_s": num(e["burnleft"]), "cooldown_left_s": num(e["cooldown"]),
                            "clock": e.get("clock")} for e in restores]
        ends = [abs(v) for e in restores for v in floats(e["enderr"])]
        phases = [abs(v) for e in restores for v in floats(e["phaseerr"])]
        out["restore_end_error_ms"] = dist(ends)
        out["restore_phase_error_ms"] = dist(phases)
        out["handoff_transit_ms"] = dist([num(e["transit"]) for e in restores])
        out["handoff_bytes"] = dist([num(e["bytes"]) for e in of("handoff-recv")])

    # The burning ticks of a player who crossed: one second apart, whichever server applied them.
    crossers = {e["id"] for e in restores}
    cadence = {}
    for who in crossers:
        ticks = sorted(num(h["at"]) for h in hits if h.get("target") == who and h.get("effect") == "Burning")
        crossing = [num(e["at"]) for e in restores if e["id"] == who]
        if len(ticks) > 2 and crossing:
            gaps = [round(b - a, 3) for a, b in zip(ticks, ticks[1:])]
            around = [round(b - a, 3) for a, b in zip(ticks, ticks[1:]) if a <= crossing[0] <= b]
            cadence[who] = {"ticks": len(ticks), "gap_min_s": min(gaps), "gap_max_s": max(gaps), "gap_across_handoff_s": around}
    if cadence:
        out["burn_cadence"] = cadence

    counts = {}
    for name in ("handoff-send", "handoff-restore", "handoff-timeout", "arrival-missing", "effect-lost", "reforward",
                 "effect-waits", "effects-caught-up", "credit-lost", "forward", "forwarded-in", "credit", "explode"):
        counts[name] = len(of(name))
    out["counts"] = counts
    return out


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    json_out = None
    if "--json" in sys.argv:
        json_out = sys.argv[sys.argv.index("--json") + 1]
        args = [a for a in args if a != json_out]
    tags = args or sorted({re.sub(r"-\d+\.log$", "", os.path.basename(p))[len("gaslab-"):]
                           for p in glob.glob(os.path.join(LOGS, "gaslab-*-[0-9].log"))})
    results = [r for r in (analyze(t) for t in tags) if r]
    for r in results:
        print(json.dumps(r, indent=1))
    if json_out:
        with open(json_out, "w", encoding="utf-8") as f:
            json.dump(results, f, indent=1)


if __name__ == "__main__":
    main()
