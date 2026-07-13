#!/usr/bin/env bash
# Deterministic reproducer for a specific soak round using its captured
# server.sync as the action-stream source.
#
# Usage:
#   scripts/repro_soak_round.sh <evidence_dir> <initial_rand_hex> \
#       <race_slot0> <race_slot1> <launch_order>
#
# Example (round 4 of soak_qt_20):
#   scripts/repro_soak_round.sh docs/soak_qt_20_round4_fail \
#       8bbfd0d3 zerg protoss agent_slot0,agent_slot1,qt_slot1,qt_slot0
#
# Every attach uses the same 10-20s stagger the original soak used
# (default 15s in this script for reproducibility). Set REPRO_STAGGER
# to override.
#
# Output goes to /tmp/repro_$(basename evidence_dir); the analyze step
# at the end compares the fresh sync-logs against the original recorded
# ones, verifying we reproduced the same divergence.

set -euo pipefail

if [ $# -lt 5 ]; then
    echo "usage: $0 <evidence_dir> <initial_rand> <race0> <race1> <order_csv>" >&2
    exit 1
fi

EVIDENCE=$1
RAND=$2
R0=$3
R1=$4
ORDER_CSV=$5
STAGGER=${REPRO_STAGGER:-15}
PLAY_SEC=${REPRO_PLAY_SEC:-300}

if [ ! -f "$EVIDENCE/server.sync" ]; then
    echo "[repro] ERROR: $EVIDENCE/server.sync not found" >&2
    exit 2
fi

REPO=$(cd "$(dirname "$0")/.." && pwd)
cd "$REPO"

OUT=/tmp/repro_$(basename "$EVIDENCE")
rm -rf "$OUT"
mkdir -p "$OUT"

echo "[repro] evidence=$EVIDENCE"
echo "[repro] initial_rand=$RAND races=$R0/$R1"
echo "[repro] launch order: $ORDER_CSV"
echo "[repro] stagger=${STAGGER}s play=${PLAY_SEC}s"
echo "[repro] out=$OUT"

pkill -f openbw_server 2>/dev/null || true
pkill -f simsc_app 2>/dev/null || true
pkill -f python_agent 2>/dev/null || true
sleep 1

# Server: production shape (no --wait-observers), fixed-initial-rand so
# the LCG chain is byte-identical to the recorded run.
nohup ./build_srv/server/openbw_server \
    --data-path original_resources \
    --map 'original_resources/(2)Bottleneck.scm' \
    --user 'slot0_p:sk-a:player:0' \
    --user 'slot1_p:sk-b:player:1' \
    --race "0=$R0" --race "1=$R1" \
    --game-speed 10 --obs-port 6114 \
    --fixed-initial-rand "$RAND" \
    --sync-log "$OUT/server.sync" \
    > "$OUT/server.log" 2>&1 &
SRV_PID=$!
echo "[repro] server PID=$SRV_PID"
sleep 2

launch_one() {
    local name=$1
    case "$name" in
        agent_slot0)
            PYTHONUNBUFFERED=1 nohup python3 -u \
                -m python_agent.agents.replay_agent sk-a --slot 0 \
                --sync-log "$EVIDENCE/server.sync" \
                --host 127.0.0.1 --port 6113 \
                > "$OUT/agent_slot0.log" 2>&1 &
            echo "  agent_slot0 (replay) PID=$!"
            ;;
        agent_slot1)
            PYTHONUNBUFFERED=1 nohup python3 -u \
                -m python_agent.agents.replay_agent sk-b --slot 1 \
                --sync-log "$EVIDENCE/server.sync" \
                --host 127.0.0.1 --port 6113 \
                > "$OUT/agent_slot1.log" 2>&1 &
            echo "  agent_slot1 (replay) PID=$!"
            ;;
        qt_slot0)
            nohup build_qt/simsc_app/simsc_app \
                --data-path original_resources \
                --map 'original_resources/(2)Bottleneck.scm' \
                --url ws://127.0.0.1:6114/observer \
                --api-key sk-a \
                --race "0=$R0" --race "1=$R1" \
                --sync-log "$OUT/qt_slot0.sync" \
                > "$OUT/qt_slot0.log" 2>&1 &
            echo "  qt_slot0 PID=$!"
            ;;
        qt_slot1)
            nohup build_qt/simsc_app/simsc_app \
                --data-path original_resources \
                --map 'original_resources/(2)Bottleneck.scm' \
                --url ws://127.0.0.1:6114/observer \
                --api-key sk-b \
                --race "0=$R0" --race "1=$R1" \
                --sync-log "$OUT/qt_slot1.sync" \
                > "$OUT/qt_slot1.log" 2>&1 &
            echo "  qt_slot1 PID=$!"
            ;;
        *)
            echo "  unknown launch entry: $name" >&2
            ;;
    esac
}

IFS=',' read -ra ORDER <<<"$ORDER_CSV"
i=0
for entry in "${ORDER[@]}"; do
    i=$((i+1))
    echo "[repro] [$i/${#ORDER[@]}] launching $entry"
    launch_one "$entry"
    if [ $i -lt ${#ORDER[@]} ]; then
        echo "[repro]   waiting ${STAGGER}s"
        sleep "$STAGGER"
    fi
done

echo "[repro] all launched; running ${PLAY_SEC}s..."
sleep "$PLAY_SEC"

echo "[repro] stopping..."
pkill -f openbw_server 2>/dev/null || true
pkill -f simsc_app 2>/dev/null || true
pkill -f python_agent 2>/dev/null || true
sleep 2

echo ""
echo "[repro] === sync log sizes ==="
wc -l "$OUT"/*.sync 2>/dev/null || true

echo ""
echo "[repro] === analyze fresh run against original evidence ==="
python3 <<PY
import os, sys
from collections import Counter

def load_lcg(path):
    out = {}
    prev = None
    with open(path) as f:
        for ln in f:
            if "\tLCG_TICK\t" in ln:
                for p in ln.rstrip().split("\t"):
                    if p.startswith("lcg="):
                        prev = p.split("=",1)[1]
            elif "\tTICK\t" in ln:
                d = {}
                for p in ln.rstrip().split("\t"):
                    if "=" in p:
                        k,v = p.split("=",1); d[k]=v
                cf = int(d.get("current_frame","-1"))
                if prev is not None: out[cf] = prev
    return out

def load_inv(path):
    out = {}
    for ln in open(path):
        if "\tINVENTORY\t" not in ln: continue
        parts = ln.rstrip().split("\t")
        try:
            f = int(parts[1]); s = parts[3]
            out[(f,s)] = "\t".join(parts[4:])
        except: pass
    return out

def game_start(path):
    for ln in open(path):
        if "GAME_START" in ln:
            for p in ln.rstrip().split("\t"):
                if p.startswith("initial_rand="):
                    return p.split("=",1)[1]
    return None

fresh_srv = "$OUT/server.sync"
orig_srv  = "$EVIDENCE/server.sync"

print(f"  original server initial_rand: {game_start(orig_srv)}")
print(f"  fresh    server initial_rand: {game_start(fresh_srv)}")

o = load_lcg(orig_srv); f = load_lcg(fresh_srv)
common = sorted(set(o) & set(f))
diffs = [cf for cf in common if o[cf] != f[cf]]
print(f"  original vs fresh SERVER LCG: {len(common)} shared, {len(diffs)} mismatches")
if diffs:
    cf = diffs[0]
    print(f"    first diff cf={cf} orig={o[cf]} fresh={f[cf]}")

oi = load_inv(orig_srv); fi = load_inv(fresh_srv)
ic = set(oi) & set(fi)
id_ = [k for k in ic if oi[k] != fi[k]]
print(f"  original vs fresh SERVER INVENTORY: {len(ic)} shared, {len(id_)} mismatches")

for obs in ["qt_slot0", "qt_slot1"]:
    orig = f"$EVIDENCE/{obs}.sync"
    fresh = f"$OUT/{obs}.sync"
    if not (os.path.exists(orig) and os.path.exists(fresh)): continue
    # server-vs-observer comparison in FRESH run
    fs = load_lcg(fresh_srv); fo = load_lcg(fresh)
    common = sorted(set(fs) & set(fo))
    diffs = [cf for cf in common if fs[cf] != fo[cf]]
    print(f"  FRESH server vs {obs}: {len(common)} shared, {len(diffs)} LCG mismatches")
PY

echo ""
echo "[repro] done. inspect $OUT/"
