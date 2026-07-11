#!/bin/bash
# repro_syncbreaker.sh -- reproduce and detect the SyncBreaker class
# of late-join sync bugs. See docs/syncbreaker.md for the full write-up.
#
# What it does per round:
#   1. Kill any leftover openbw processes.
#   2. Start openbw_server with sync-log enabled.
#   3. Start two agents (they play immediately with 0 observers).
#   4. Wait 10s so the action log has meaningful catchup content.
#   5. Launch two observers 0.4s apart.
#   6. Play $PLAY_SECS seconds.
#   7. Kill everything.
#   8. Diff the three sync-logs and classify each round:
#        - PASS:            no INVENTORY disagreements at all
#        - PASS_COSMETIC:   only mineral/gas drift (timing wobble)
#        - FAIL_REAL:       unit-count divergence (SyncBreaker fired!)
#   9. On FAIL_REAL keep the round's logs under ${round}_FAIL_REAL/
#      for post-mortem. On PASS, delete them.
#
# Usage:
#   scripts/repro_syncbreaker.sh
#   ROUNDS=20 PLAY_SECS=30 scripts/repro_syncbreaker.sh
#   PORT=6114 scripts/repro_syncbreaker.sh          # WS transport
#   PORT=6112 scripts/repro_syncbreaker.sh          # raw TCP (pre-2026-07)
#
# The default PORT=6114 matches the current WS observer transport.
# Change to 6112 (or whatever your build binds) to test raw TCP.

set -u

# ---- config ----
REPO="$(cd "$(dirname "$0")/.." && pwd)"
ALICE=${ALICE:-sk-LYvXIzRDaDEe8GTlPgyf8eMxAyamUJt_Ig2413DbEjw}
BOB=${BOB:-sk-anYTfuY-QL9szAzIlvtv44RxpgJlJPC1ocqIA26qpf0}
MAP=${MAP:-"$REPO/original_resources/(2)Bottleneck.scm"}
ROUNDS=${ROUNDS:-10}
PLAY_SECS=${PLAY_SECS:-25}
PORT=${PORT:-6114}
OUT=${OUT:-/tmp/syncbreaker_repro}

mkdir -p "$OUT"

pass=0
fail_real=0
fail_cosmetic=0

for round in $(seq 1 $ROUNDS); do
    rdir="$OUT/round_$round"
    rm -rf "$rdir"; mkdir -p "$rdir"

    pkill -f openbw_server >/dev/null 2>&1
    pkill -f openbw_observer >/dev/null 2>&1
    pkill -f "agents\." >/dev/null 2>&1
    sleep 1

    echo "==== round $round/$ROUNDS ===="

    # 1. server
    ( cd "$REPO" && \
      nohup ./build_srv/server/openbw_server \
        --map "$MAP" --data-path original_resources \
        --users test_resources/users.json \
        --race "0=zerg" --race "1=zerg" \
        --game-speed 10 \
        --sync-log "$rdir/server.sync" \
        > "$rdir/server.log" 2>&1 & )
    for i in $(seq 1 40); do
        nc -z 127.0.0.1 6113 2>/dev/null && \
          nc -z 127.0.0.1 $PORT 2>/dev/null && break
        sleep 0.25
    done
    echo "  server ready (obs port $PORT)"

    # 2. agents
    ( cd "$REPO" && \
      PYTHONUNBUFFERED=1 nohup python3 -m python_agent.agents.z_agent_v5 \
        "$ALICE" --interval-sec 1.0 > "$rdir/agent_alice.log" 2>&1 & )
    ( cd "$REPO" && \
      PYTHONUNBUFFERED=1 nohup python3 -m python_agent.agents.z_agent_v5 \
        "$BOB" --interval-sec 1.0 > "$rdir/agent_bob.log" 2>&1 & )
    echo "  agents started"

    # 3. Wait so the action log has content in the catchup vulnerable window
    sleep 10

    # 4. observers -- 0.4s apart matches the "hot" window
    ( cd "$REPO" && \
      nohup ./build_srv/ui/openbw_observer \
        --map "$MAP" --data-path original_resources \
        --server 127.0.0.1:$PORT --api-key "$ALICE" \
        --race "0=zerg" --race "1=zerg" \
        --sync-log "$rdir/observer_alice.sync" \
        > "$rdir/observer_alice.log" 2>&1 & )
    sleep 0.4
    ( cd "$REPO" && \
      nohup ./build_srv/ui/openbw_observer \
        --map "$MAP" --data-path original_resources \
        --server 127.0.0.1:$PORT --api-key "$BOB" \
        --race "0=zerg" --race "1=zerg" \
        --sync-log "$rdir/observer_bob.sync" \
        > "$rdir/observer_bob.log" 2>&1 & )
    echo "  observers started"

    # 5. Play
    sleep "$PLAY_SECS"

    # 6. Kill
    pkill -f openbw_server >/dev/null 2>&1
    pkill -f openbw_observer >/dev/null 2>&1
    pkill -f "agents\." >/dev/null 2>&1
    sleep 1

    # 7. Analyze -- classify diffs into cosmetic vs REAL.
    result=$(python3 <<PYEOF
def load(p):
    out = {}
    try: f = open(p)
    except FileNotFoundError: return out
    for ln in f:
        if "INVENTORY" not in ln: continue
        parts = ln.rstrip().split("\t")
        frame = int(parts[1]); slot = int(parts[3].split("=")[1])
        out[(frame, slot)] = "\t".join(parts[4:])
    return out

srv = load("$rdir/server.sync")
oa  = load("$rdir/observer_alice.sync")
ob  = load("$rdir/observer_bob.sync")

def classify(a, b):
    common = sorted(set(a) & set(b))
    min_only = 0; real = 0
    for k in common:
        if a[k] == b[k]: continue
        # Strip min= and gas=. Unit-counts-only comparison.
        au = "\t".join(a[k].split("\t")[2:])
        bu = "\t".join(b[k].split("\t")[2:])
        if au == bu: min_only += 1
        else: real += 1
    return len(common), min_only, real

a_sh, a_min, a_real = classify(srv, oa)
b_sh, b_min, b_real = classify(srv, ob)

verdict = "PASS"
if a_real > 0 or b_real > 0:
    verdict = "FAIL_REAL"
elif a_min > 0 or b_min > 0:
    verdict = "PASS_COSMETIC"

print(f"alice_shared={a_sh} alice_min_only={a_min} alice_REAL={a_real} "
      f"bob_shared={b_sh} bob_min_only={b_min} bob_REAL={b_real} "
      f"verdict={verdict}")
PYEOF
)
    echo "  $result"

    if [[ "$result" == *"verdict=PASS"* ]]; then
        pass=$((pass + 1))
        rm -rf "$rdir"
    elif [[ "$result" == *"verdict=FAIL_REAL"* ]]; then
        fail_real=$((fail_real + 1))
        mv "$rdir" "${rdir}_FAIL_REAL"
        echo "  round $round: **FAIL_REAL** -- logs kept at ${rdir}_FAIL_REAL"
    else
        fail_cosmetic=$((fail_cosmetic + 1))
        rm -rf "$rdir"
    fi
done

echo ""
echo "==== summary ===="
echo "  passed:               $pass / $ROUNDS"
echo "  fail (real):          $fail_real / $ROUNDS"
echo "  fail (cosmetic-only): $fail_cosmetic / $ROUNDS"
echo ""
if (( fail_real > 0 )); then
    echo "SyncBreaker fired in $fail_real rounds. See ${OUT}/round_*_FAIL_REAL/"
    exit 1
else
    echo "No SyncBreaker instance detected across $ROUNDS rounds."
    exit 0
fi
