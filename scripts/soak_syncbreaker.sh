#!/bin/bash
# soak_syncbreaker.sh -- long-duration SyncBreaker soak test.
#
# Per round:
#   1. Kill leftovers.
#   2. Pick random race for each slot (zerg/terran/protoss).
#   3. Start server with those races + sync-log.
#   4. Wait for server to bind ports.
#   5. Randomize the launch order of 4 entities:
#      2 agents (matching each slot's race) + 2 observers.
#      Launch them one at a time with 20-second intervals.
#   6. Play for PLAY_SECS after the last launch.
#   7. Kill everything.
#   8. Analyze sync-logs, classify PASS / PASS_COSMETIC / FAIL_REAL.
#   9. Append one line to results.tsv, keep FAIL_REAL logs on disk,
#      delete PASS logs to save space.
#  10. Loop.
#
# The 20-second staggering is deliberately longer than the earlier
# repro's 0.4s. It stresses "catchup while game is running long"
# rather than the tight race window. Long staggering exercises
# large catchup bundles (the message_t buffer bug lived there).
#
# Usage:
#   scripts/soak_syncbreaker.sh                 # default: 1 hour of testing
#   TOTAL_SECS=7200 scripts/soak_syncbreaker.sh # 2 hours
#   PLAY_SECS=180  scripts/soak_syncbreaker.sh  # 3-min rounds instead of 5
#   OUT=/tmp/soak-2 scripts/soak_syncbreaker.sh # different output dir

set -u

# ---- config ----
REPO="$(cd "$(dirname "$0")/.." && pwd)"
ALICE=${ALICE:-sk-LYvXIzRDaDEe8GTlPgyf8eMxAyamUJt_Ig2413DbEjw}
BOB=${BOB:-sk-anYTfuY-QL9szAzIlvtv44RxpgJlJPC1ocqIA26qpf0}
SPEC1=${SPEC1:-sk-nd1eLrQ4yEMcQM3nBcf1c34fK8xXQvPaxlUr5rgdjLE7}
# Observer #2 also uses the alice or bob key (players are allowed to
# observe their own game via the same key). This works because auth
# allows player-role users to open observer connections too.
SPEC2=${SPEC2:-$ALICE}
MAP=${MAP:-"$REPO/original_resources/(2)Bottleneck.scm"}

# 5-min play per round; user asked for this specifically.
PLAY_SECS=${PLAY_SECS:-300}
# 20-second interval between launches; user asked for this.
STAGGER=${STAGGER:-20}
# Total wallclock target. Default 1 hour; pass TOTAL_SECS=7200 for 2h.
TOTAL_SECS=${TOTAL_SECS:-3600}
PORT=${PORT:-6114}
OUT=${OUT:-/tmp/soak_syncbreaker}

mkdir -p "$OUT"

# Map from race name -> latest agent module. Only zerg/terran/protoss
# have full-race agents; we use their v4/v5 flagships.
# macOS ships bash 3.2 which lacks associative arrays, so use a
# function instead.
agent_module_for_race() {
    case "$1" in
        zerg)    echo "python_agent.agents.z_agent_v5" ;;
        terran)  echo "python_agent.agents.t_agent_v5" ;;
        protoss) echo "python_agent.agents.p_agent_v4" ;;
        *)       echo "unknown_race_$1"; return 1 ;;
    esac
}

# Results file -- one line per round, tab-separated, human-readable.
RESULTS="$OUT/results.tsv"
if [[ ! -f "$RESULTS" ]]; then
    printf "round\tstart_ts\trace0\trace1\tlaunch_order\tverdict\talice_shared\talice_min_only\talice_real\tbob_shared\tbob_min_only\tbob_real\tserver_frames\tnotes\n" > "$RESULTS"
fi

kill_all() {
    pkill -f openbw_server >/dev/null 2>&1
    pkill -f openbw_observer >/dev/null 2>&1
    pkill -f "agents\." >/dev/null 2>&1
    sleep 1
}

pick_race() {
    local races=(zerg terran protoss)
    echo "${races[$((RANDOM % 3))]}"
}

start_ts=$(date +%s)
elapsed() { echo $(($(date +%s) - start_ts)); }

round=0
pass=0
pass_cosmetic=0
fail_real=0

# Compact per-launch entity spec. Each entry is TYPE:SLOT:RACE.
# Types: A=agent, O=observer. Slot is 0 or 1. Race matches the slot.
# The bash "shuf" trick used here gives us a random permutation of
# the 4 entities to launch, with 20 s between each -- so all four
# orders like O0 A0 A1 O1 or A1 O0 O1 A0 come up over the run.

while true; do
    now=$(elapsed)
    if (( now >= TOTAL_SECS )); then
        echo ""
        echo "==== soak complete after ${now}s ===="
        break
    fi
    round=$((round + 1))
    round_start=$(date +%s)
    rdir="$OUT/round_$(printf '%03d' $round)"
    rm -rf "$rdir"; mkdir -p "$rdir"

    kill_all

    RACE0=$(pick_race)
    RACE1=$(pick_race)
    # macOS doesn't ship `shuf` -- use python for portable shuffling.
    ORDER=$(python3 -c "import random; xs=['A:0','A:1','O:alice','O:spec']; random.shuffle(xs); print(' '.join(xs))")
    echo ""
    echo "==== round $round  race0=$RACE0 race1=$RACE1 order=[$ORDER]  elapsed=${now}s ===="
    echo "  round dir: $rdir"

    # 1. Start server.
    ( cd "$REPO" && \
      nohup ./build_srv/server/openbw_server \
        --map "$MAP" --data-path original_resources \
        --users test_resources/users.json \
        --race "0=$RACE0" --race "1=$RACE1" \
        --game-speed 42 \
        --sync-log "$rdir/server.sync" \
        > "$rdir/server.log" 2>&1 & )

    # Wait for server ports.
    ready=0
    for i in $(seq 1 60); do
        if nc -z 127.0.0.1 6113 2>/dev/null && nc -z 127.0.0.1 $PORT 2>/dev/null; then
            ready=1; break
        fi
        sleep 0.25
    done
    if (( !ready )); then
        echo "  ERROR: server never bound both ports; aborting round"
        mv "$rdir" "${rdir}_SERVER_FAIL"
        # Log a row so results.tsv has this too.
        printf "%d\t%s\t%s\t%s\t[%s]\tSERVER_FAIL\t0\t0\t0\t0\t0\t0\t0\tserver bind timeout\n" \
            "$round" "$(date -u -r $round_start +%FT%TZ)" \
            "$RACE0" "$RACE1" "$ORDER" \
            >> "$RESULTS"
        continue
    fi
    echo "  server ready (obs port $PORT)"

    # 2. Launch entities in the shuffled order, one every $STAGGER seconds.
    ord_arr=($ORDER)
    launch_idx=0
    for entity in "${ord_arr[@]}"; do
        launch_idx=$((launch_idx + 1))
        case "$entity" in
            A:0)
                agent_mod=$(agent_module_for_race "$RACE0")
                ( cd "$REPO" && \
                  PYTHONUNBUFFERED=1 nohup python3 -m "$agent_mod" \
                    "$ALICE" --host 127.0.0.1 --port 6113 --interval-sec 1.0 \
                    > "$rdir/agent_alice.log" 2>&1 & )
                echo "  [+${launch_idx}] agent alice ($RACE0 / $agent_mod)"
                ;;
            A:1)
                agent_mod=$(agent_module_for_race "$RACE1")
                ( cd "$REPO" && \
                  PYTHONUNBUFFERED=1 nohup python3 -m "$agent_mod" \
                    "$BOB" --host 127.0.0.1 --port 6113 --interval-sec 1.0 \
                    > "$rdir/agent_bob.log" 2>&1 & )
                echo "  [+${launch_idx}] agent bob ($RACE1 / $agent_mod)"
                ;;
            O:alice)
                ( cd "$REPO" && \
                  nohup ./build_srv/ui/openbw_observer \
                    --map "$MAP" --data-path original_resources \
                    --server 127.0.0.1:$PORT --api-key "$ALICE" \
                    --race "0=$RACE0" --race "1=$RACE1" \
                    --sync-log "$rdir/obs_alice.sync" \
                    > "$rdir/obs_alice.log" 2>&1 & )
                echo "  [+${launch_idx}] observer (alice key -> perspective slot 0)"
                ;;
            O:spec)
                ( cd "$REPO" && \
                  nohup ./build_srv/ui/openbw_observer \
                    --map "$MAP" --data-path original_resources \
                    --server 127.0.0.1:$PORT --api-key "$SPEC1" \
                    --race "0=$RACE0" --race "1=$RACE1" \
                    --sync-log "$rdir/obs_spec.sync" \
                    > "$rdir/obs_spec.log" 2>&1 & )
                echo "  [+${launch_idx}] observer (spec key -> full vision)"
                ;;
        esac
        # Interval between launches, EXCEPT after the last one -- go
        # straight to PLAY_SECS.
        if (( launch_idx < ${#ord_arr[@]} )); then
            sleep "$STAGGER"
        fi
    done

    # 3. Play.
    echo "  playing for ${PLAY_SECS}s..."
    sleep "$PLAY_SECS"

    # 4. Read the server's final frame count before killing.
    server_frames=$(grep -oE "frame=[0-9]+" "$rdir/server.log" | tail -1 | cut -d= -f2)
    server_frames=${server_frames:-0}
    echo "  server final frame: $server_frames"

    # Also check for known crash signatures on the server side.
    crash_notes=""
    if grep -q "too much data" "$rdir/server.log" 2>/dev/null; then
        crash_notes="server_msg_buf_overflow"
    elif grep -qE "libc\+\+abi: terminating|exception|abort" "$rdir/server.log" 2>/dev/null; then
        crash_notes="server_crash"
    fi

    # 5. Stop.
    kill_all

    # 6. Analyze. We compare each observer's sync-log against the
    # server's. INVENTORY lines are keyed by (frame, slot). Diff
    # into cosmetic (mineral/gas timing wobble) vs real (unit-count
    # divergence).
    result=$(python3 <<PYEOF
def load(p):
    out = {}
    try: f = open(p)
    except FileNotFoundError: return out
    for ln in f:
        if "INVENTORY" not in ln: continue
        parts = ln.rstrip().split("\t")
        try:
            frame = int(parts[1])
            slot = int(parts[3].split("=")[1])
            out[(frame, slot)] = "\t".join(parts[4:])
        except (IndexError, ValueError):
            pass
    return out

srv = load("$rdir/server.sync")
oa  = load("$rdir/obs_alice.sync")
ob  = load("$rdir/obs_spec.sync")

def classify(server, obs):
    # Server logs INVENTORY on frame N==300k; observer's sync_frame
    # counter runs one frame ahead when its own INVENTORY sink fires
    # (see feedback_sync_log_off_by_one memory). Match observer's
    # frame F against server's frame F+1 OR F-1 OR F, whichever exists.
    min_only = 0; real = 0; matched = 0
    for k, ov in obs.items():
        frame, slot = k
        sv = server.get(k) or server.get((frame+1, slot)) or server.get((frame-1, slot))
        if sv is None: continue
        matched += 1
        if ov == sv: continue
        # Strip min= and gas= (parts[0], parts[1]). Compare only unit
        # counts (parts[2:]).
        ou = "\t".join(ov.split("\t")[2:])
        su = "\t".join(sv.split("\t")[2:])
        if ou == su: min_only += 1
        else: real += 1
    return matched, min_only, real

a_sh, a_min, a_real = classify(srv, oa)
b_sh, b_min, b_real = classify(srv, ob)

verdict = "PASS"
if a_real > 0 or b_real > 0:
    verdict = "FAIL_REAL"
elif a_min > 0 or b_min > 0:
    verdict = "PASS_COSMETIC"

# If neither observer has ANY shared INVENTORY rows with the server,
# something more fundamental failed (crash mid-round, observer never
# authed, etc). Escalate to a different verdict so it doesn't hide
# in the PASS bucket.
if a_sh == 0 and b_sh == 0:
    verdict = "NO_DATA"

print(f"alice_shared={a_sh} alice_min_only={a_min} alice_REAL={a_real} "
      f"bob_shared={b_sh} bob_min_only={b_min} bob_REAL={b_real} "
      f"verdict={verdict}")
PYEOF
)
    echo "  $result"

    # 7. Parse the python output back for the results table.
    verdict=$(echo "$result" | sed -n 's/.*verdict=\([A-Z_]*\).*/\1/p')
    a_sh=$(echo "$result" | sed -n 's/.*alice_shared=\([0-9]*\).*/\1/p')
    a_min=$(echo "$result" | sed -n 's/.*alice_min_only=\([0-9]*\).*/\1/p')
    a_real=$(echo "$result" | sed -n 's/.*alice_REAL=\([0-9]*\).*/\1/p')
    b_sh=$(echo "$result" | sed -n 's/.*bob_shared=\([0-9]*\).*/\1/p')
    b_min=$(echo "$result" | sed -n 's/.*bob_min_only=\([0-9]*\).*/\1/p')
    b_real=$(echo "$result" | sed -n 's/.*bob_REAL=\([0-9]*\).*/\1/p')

    printf "%d\t%s\t%s\t%s\t[%s]\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n" \
        "$round" "$(date -u -r $round_start +%FT%TZ)" \
        "$RACE0" "$RACE1" "$ORDER" "$verdict" \
        "$a_sh" "$a_min" "$a_real" \
        "$b_sh" "$b_min" "$b_real" \
        "$server_frames" "$crash_notes" \
        >> "$RESULTS"

    case "$verdict" in
        PASS)
            pass=$((pass + 1))
            rm -rf "$rdir"
            ;;
        PASS_COSMETIC)
            pass_cosmetic=$((pass_cosmetic + 1))
            rm -rf "$rdir"
            ;;
        FAIL_REAL|NO_DATA|"")
            fail_real=$((fail_real + 1))
            mv "$rdir" "${rdir}_${verdict:-UNKNOWN}"
            echo "  round $round: **${verdict:-UNKNOWN}** -- logs kept at ${rdir}_${verdict:-UNKNOWN}"
            ;;
    esac

    # Running tally so tail -f results.tsv is informative even for
    # very long soaks.
    echo "  running tally: PASS=$pass PASS_COSMETIC=$pass_cosmetic FAIL=$fail_real / $round"
done

echo ""
echo "==== soak summary ===="
echo "  rounds run:           $round"
echo "  passed:               $pass"
echo "  pass_cosmetic only:   $pass_cosmetic"
echo "  fail (real):          $fail_real"
echo "  results table:        $RESULTS"
