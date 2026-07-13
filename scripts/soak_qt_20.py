#!/usr/bin/env python3
"""
20-round Qt observer soak test.

Each round: pick random races for both slots, launch server (waits for
2 observers), then launch { agent_slot0, agent_slot1, qt_slot0, qt_slot1 }
in RANDOM ORDER with a RANDOM 10..20 s delay between each. Once all four
are up and the sim is running, let it play for 5 minutes at --game-speed
10, then stop everything and analyze the three sync-logs (server + 2 Qt
observers) for divergence.

After each round we print a one-line PASS/FAIL and continue to the next
round automatically (per user's "without asking" instruction).
"""
import os, sys, time, random, subprocess, socket, json, threading
from pathlib import Path
from collections import Counter

REPO = Path(__file__).resolve().parent.parent
os.chdir(REPO)
OUTDIR = Path("/tmp/soak_qt_20")
OUTDIR.mkdir(exist_ok=True)

RACES = ["terran", "zerg", "protoss"]
AGENT_MODULE = {
    "terran":  "python_agent.agents.t_agent_v5",
    "zerg":    "python_agent.agents.z_agent_v5",
    "protoss": "python_agent.agents.p_agent_v4",
}
MAP = "original_resources/(2)Bottleneck.scm"

def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)

def kill_all():
    for pat in ["openbw_server", "simsc_app", "python_agent"]:
        subprocess.run(["pkill", "-f", pat],
                       stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
    time.sleep(1.5)

def wait_port(port, timeout):
    """Block until a TCP port is accepting connections."""
    end = time.time() + timeout
    while time.time() < end:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1):
                return True
        except OSError:
            time.sleep(0.3)
    return False

def wait_pattern_in(path, pattern, timeout):
    end = time.time() + timeout
    while time.time() < end:
        try:
            if pattern in Path(path).read_text():
                return True
        except FileNotFoundError:
            pass
        time.sleep(0.5)
    return False

# --- sync-log parsers (same shape used by all our analyses) ---

def load_lcg_by_cf(path):
    out = {}; prev = None
    for ln in open(path):
        if "\tLCG_TICK\t" in ln:
            for p in ln.rstrip().split("\t"):
                if p.startswith("lcg="):
                    prev = p.split("=", 1)[1]
        elif "\tTICK\t" in ln:
            d = {}
            for p in ln.rstrip().split("\t"):
                if "=" in p:
                    k, v = p.split("=", 1); d[k] = v
            cf = int(d.get("current_frame", "-1"))
            if prev is not None:
                out[cf] = prev
    return out

def load_inv(path):
    out = {}
    for ln in open(path):
        if "\tINVENTORY\t" not in ln:
            continue
        parts = ln.rstrip().split("\t")
        try:
            frame = int(parts[1]); slot = parts[3]
            out[(frame, slot)] = "\t".join(parts[4:])
        except Exception:
            pass
    return out

def load_apply(path):
    out = Counter()
    for ln in open(path):
        if "\tAGENT_APPLY\t" not in ln:
            continue
        d = {}
        for p in ln.rstrip().split("\t"):
            if "=" in p:
                k, v = p.split("=", 1); d[k] = v
        out[(d.get("target_frame"), d.get("slot"), d.get("bytes"))] += 1
    return out

def load_game_start(path):
    for ln in open(path):
        if "GAME_START" in ln:
            for p in ln.rstrip().split("\t"):
                if p.startswith("initial_rand="):
                    return p.split("=", 1)[1]
    return None

def analyze_round(dir_path):
    srv = dir_path / "server.sync"
    obs = [("qt_slot0", dir_path / "qt_slot0.sync"),
           ("qt_slot1", dir_path / "qt_slot1.sync")]
    r = {"initial_rand": {}, "lcg_check": {}, "inv_check": {},
         "apply_check": {}, "cross_check": None, "notes": [],
         "server_frames": None, "pass": True}
    if not srv.exists() or srv.stat().st_size == 0:
        r["pass"] = False
        r["notes"].append("server.sync missing/empty")
        return r
    r["initial_rand"]["server"] = load_game_start(srv)
    srv_lcg = load_lcg_by_cf(srv)
    srv_inv = load_inv(srv)
    srv_ap = load_apply(srv)
    r["server_frames"] = (min(srv_lcg), max(srv_lcg)) if srv_lcg else None
    obs_lcg = {}
    for name, path in obs:
        if not path.exists() or path.stat().st_size == 0:
            r["notes"].append(f"{name}.sync missing/empty")
            r["pass"] = False
            continue
        r["initial_rand"][name] = load_game_start(path)
        o_lcg = load_lcg_by_cf(path); o_inv = load_inv(path); o_ap = load_apply(path)
        obs_lcg[name] = o_lcg
        common = sorted(set(srv_lcg) & set(o_lcg))
        lcg_diff = sum(1 for cf in common if srv_lcg[cf] != o_lcg[cf])
        inv_common = set(srv_inv) & set(o_inv)
        inv_diff = sum(1 for k in inv_common if srv_inv[k] != o_inv[k])
        miss = sum(1 for k, c in srv_ap.items() if o_ap[k] < c)
        extra = sum(1 for k, c in o_ap.items() if srv_ap[k] < c)
        r["lcg_check"][name] = {"shared": len(common), "mismatches": lcg_diff}
        r["inv_check"][name] = {"shared": len(inv_common), "mismatches": inv_diff}
        r["apply_check"][name] = {"srv": sum(srv_ap.values()),
                                    "obs": sum(o_ap.values()),
                                    "missing": miss, "spurious": extra}
        # Missing actions are OK ONLY if observer stopped early (its own
        # shutdown), i.e. its max frame < server's max frame. Spurious
        # actions never OK.
        if lcg_diff or inv_diff or extra:
            r["pass"] = False
        if miss > 0 and o_lcg and max(o_lcg) >= max(srv_lcg):
            # observer supposedly kept up but missed actions -- that's a real fail
            r["pass"] = False
            r["notes"].append(f"{name} missing={miss} but max_cf covered")
    if len(obs_lcg) == 2:
        keys = list(obs_lcg)
        common = sorted(set(obs_lcg[keys[0]]) & set(obs_lcg[keys[1]]))
        diff = sum(1 for cf in common
                    if obs_lcg[keys[0]][cf] != obs_lcg[keys[1]][cf])
        r["cross_check"] = {"shared": len(common), "mismatches": diff}
        if diff:
            r["pass"] = False
    rands = [v for v in r["initial_rand"].values() if v]
    if len(set(rands)) > 1:
        r["pass"] = False
        r["notes"].append(f"initial_rand mismatch: {r['initial_rand']}")
    return r


def spawn_agent_when_ready(module, key, log_path):
    """Wait for agent WS port to be listening, then start the agent."""
    if not wait_port(6113, timeout=240):
        log(f"  WARN: agent port never opened for {module}")
        return
    p = subprocess.Popen(
        ["python3", "-u", "-m", module, key,
         "--host", "127.0.0.1", "--port", "6113",
         "--interval-sec", "0.5", "--base-target", "2"],
        stdout=open(log_path, "w"),
        stderr=subprocess.STDOUT,
        env={**os.environ, "PYTHONUNBUFFERED": "1"},
    )
    # keep a reference so process isn't gc'd
    _agent_procs.append(p)

_agent_procs = []

def run_round(N):
    dir_path = OUTDIR / f"round_{N:02d}"
    dir_path.mkdir(exist_ok=True)
    for f in dir_path.iterdir():
        f.unlink()
    _agent_procs.clear()

    race0 = random.choice(RACES)
    race1 = random.choice(RACES)
    log(f"ROUND {N}/20: slot0={race0} vs slot1={race1}")

    kill_all()

    # 1. Server. Production shape: NO --wait-observers, so game starts
    #    immediately. Agents and observers can attach at any subsequent
    #    time; the server ships id_catchup_data to bring them up to speed.
    #    This is what the k8s deployment runs.
    srv_proc = subprocess.Popen([
        "./build_srv/server/openbw_server",
        "--data-path", "original_resources",
        "--map", MAP,
        "--user", "slot0_p:sk-a:player:0",
        "--user", "slot1_p:sk-b:player:1",
        "--race", f"0={race0}", "--race", f"1={race1}",
        "--game-speed", "10", "--obs-port", "6114",
        "--sync-log", str(dir_path / "server.sync"),
    ], stdout=open(dir_path / "server.log", "w"), stderr=subprocess.STDOUT)
    time.sleep(1.5)

    # 2. Four task launchers (spawn each on a thread so we don't block on
    #    the agent-port wait when random order puts an agent first).
    def make_qt(key, sync_path, log_path):
        def go():
            p = subprocess.Popen([
                "build_qt/simsc_app/simsc_app",
                "--data-path", "original_resources",
                "--map", MAP,
                "--url", "ws://127.0.0.1:6114/observer",
                "--api-key", key,
                "--race", f"0={race0}", "--race", f"1={race1}",
                "--sync-log", str(sync_path),
            ], stdout=open(log_path, "w"), stderr=subprocess.STDOUT)
            _agent_procs.append(p)
        return go

    tasks = [
        ("qt_slot0",    make_qt("sk-a", dir_path / "qt_slot0.sync", dir_path / "qt_slot0.log")),
        ("qt_slot1",    make_qt("sk-b", dir_path / "qt_slot1.sync", dir_path / "qt_slot1.log")),
        ("agent_slot0", lambda: spawn_agent_when_ready(AGENT_MODULE[race0], "sk-a",
                                                        dir_path / "agent_slot0.log")),
        ("agent_slot1", lambda: spawn_agent_when_ready(AGENT_MODULE[race1], "sk-b",
                                                        dir_path / "agent_slot1.log")),
    ]
    random.shuffle(tasks)
    launch_order = [t[0] for t in tasks]
    log(f"  launch order: {launch_order}")

    # Give the server a random 5..15 s head-start so every attach is a
    # late-join in a game that's already been running. This is the shape
    # the k8s deployment sees: pods boot, sim starts, users attach later.
    initial_delay = random.randint(5, 15)
    log(f"  initial pre-attach delay: {initial_delay}s (sim runs alone)")
    time.sleep(initial_delay)

    threads = []
    for i, (name, fn) in enumerate(tasks):
        t = threading.Thread(target=fn, daemon=True)
        threads.append((name, t))
        t.start()
        log(f"  [{i+1}/4] spawned launcher: {name}")
        if i < 3:
            delay = random.randint(10, 20)
            log(f"    waiting {delay}s before next attach")
            time.sleep(delay)

    # Give launchers a bit to finish (agent port wait can be up to ~30s
    # after last observer connects and game_started fires)
    for name, t in threads:
        t.join(timeout=60)

    # 3. Wait for sim actually running (game_started + several frames)
    if not wait_pattern_in(dir_path / "server.log", "game started", timeout=60):
        log("  ERROR: server never reported 'game started' — killing round")
        kill_all()
        return {"round": N, "pass": False,
                "notes": ["server never reached game_started"],
                "races": {"slot0": race0, "slot1": race1},
                "launch_order": launch_order}

    log("  game running; sleeping 5 min...")
    time.sleep(300)

    log("  killing all and analyzing sync logs...")
    kill_all()
    time.sleep(1)

    r = analyze_round(dir_path)
    r["round"] = N
    r["races"] = {"slot0": race0, "slot1": race1}
    r["launch_order"] = launch_order
    with open(dir_path / "analysis.json", "w") as f:
        json.dump(r, f, indent=2, default=str)
    return r


def print_round_summary(r):
    status = "PASS" if r["pass"] else "FAIL"
    log(f"ROUND {r['round']} {status}")
    log(f"  races: {r['races']} launch_order: {r['launch_order']}")
    log(f"  initial_rand: {r.get('initial_rand')}")
    if r.get("server_frames"):
        log(f"  server frames: {r['server_frames'][0]}..{r['server_frames'][1]}")
    for name, v in r.get("lcg_check", {}).items():
        log(f"  {name} LCG: shared={v['shared']} mismatches={v['mismatches']}")
    for name, v in r.get("inv_check", {}).items():
        log(f"  {name} INVENTORY: shared={v['shared']} mismatches={v['mismatches']}")
    for name, v in r.get("apply_check", {}).items():
        log(f"  {name} AGENT_APPLY: srv={v['srv']} obs={v['obs']} "
            f"missing={v['missing']} spurious={v['spurious']}")
    if r.get("cross_check") is not None:
        log(f"  cross-observer LCG: shared={r['cross_check']['shared']} "
            f"mismatches={r['cross_check']['mismatches']}")
    if r.get("notes"):
        log(f"  NOTES: {r['notes']}")


def main():
    random.seed()  # non-deterministic to explore random launch orders
    all_results = []
    for N in range(1, 21):
        try:
            r = run_round(N)
        except Exception as e:
            import traceback
            log(f"ROUND {N} EXCEPTION: {e}")
            traceback.print_exc()
            r = {"round": N, "pass": False, "notes": [f"exception: {e}"]}
        all_results.append(r)
        print_round_summary(r)
        # Persist rolling summary after every round
        with open(OUTDIR / "summary.json", "w") as f:
            json.dump(all_results, f, indent=2, default=str)
        log(f"===== ROUND {N} DONE =====")

    kill_all()
    n_pass = sum(1 for r in all_results if r.get("pass"))
    n_fail = sum(1 for r in all_results if not r.get("pass"))
    log("")
    log(f"FINAL: pass={n_pass}/20  fail={n_fail}/20")

if __name__ == "__main__":
    main()
