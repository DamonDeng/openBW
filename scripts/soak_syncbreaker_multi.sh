#!/bin/bash
# soak_syncbreaker_multi.sh -- next-level SyncBreaker soak: multi-player
# maps (2/4/6/8), random race per slot, random launch order for the
# 2N entities (N agents + N observers), 20s stagger, 5-min play.
#
# Per round:
#   1. Pick a random 2/4/6/8-player map from original_resources/.
#   2. Pick a random race for each of the N slots.
#   3. Start server with those races + sync-log.
#   4. Wait for server ports.
#   5. Build the launch list of 2N entities:
#      N agents (A:0..N-1) + N observers (O:0..N-1). Shuffle. Launch
#      one every STAGGER seconds. Each observer uses that slot's own
#      player key -> gets slot-N fog-of-war.
#   6. Play PLAY_SECS after the last launch.
#   7. Kill everything.
#   8. Analyze: compare each observer's sync-log INVENTORY entries
#      against the server's (with +/-1 frame tolerance) for every
#      slot. A round is PASS_COSMETIC if any observer has min/gas
#      drift only; PASS if no drift at all; FAIL_REAL if any
#      observer's unit counts diverge from server's.
#   9. Append one line to results.tsv, keep FAIL_REAL logs on disk,
#      delete PASS logs to save space.
#  10. Loop until TOTAL_SECS elapsed.
#
# Usage:
#   scripts/soak_syncbreaker_multi.sh                 # 1h default
#   TOTAL_SECS=7200 scripts/soak_syncbreaker_multi.sh # 2h
#   PLAYERS_FILTER="2,4" scripts/soak_syncbreaker_multi.sh   # only 2/4-player maps
#
# NOTE: this script scales up quickly. 8-player = 8 agents + 8 observers
# = 16 processes + 1 server per round, plus 300s play at speed=10 is
# CPU-intensive. Watch Activity Monitor.

set -u

# ---- config ----
REPO="$(cd "$(dirname "$0")/.." && pwd)"
# 8 player-role keys in test_resources/users.json (alice..henry).
KEYS=(
    sk-LYvXIzRDaDEe8GTlPgyf8eMxAyamUJt_Ig2413DbEjw   # alice   slot 0
    sk-anYTfuY-QL9szAzIlvtv44RxpgJlJPC1ocqIA26qpf0   # bob     slot 1
    sk-jdDXKI5p67vcaxRx9MTGs6H0qnAm5pGmi270C3uJUfY   # carol   slot 2
    sk-8tvfxEmYOmBPXi6ei_FQrwFmLqfm-gkMbVBpchBxOqo   # dave    slot 3
    sk-8_RWcJuffoj0cICWlN1kjUZtCa7G4j1yR4bs_8sWTIM   # eve     slot 4
    sk-MJyi6EsR2ZraWvk6Lp-N7V4qbqeFG_ymOzDcetA1i7g   # frank   slot 5
    sk-9AXKo69ndh0U9pimUDijIzm5B3pN8ucFZAk28QhN4jY   # grace   slot 6
    sk-q9kxrOyBOnUefMbS1y0k0XgoEz2uI9PnZJIPtJJtgXI   # henry   slot 7
)
NAMES=(alice bob carol dave eve frank grace henry)

PLAY_SECS=${PLAY_SECS:-300}
STAGGER=${STAGGER:-20}
TOTAL_SECS=${TOTAL_SECS:-3600}
GAME_SPEED=${GAME_SPEED:-10}
PORT=${PORT:-6114}
OUT=${OUT:-/tmp/soak_syncbreaker_multi}
# Comma-separated list of allowed player-counts. Default: all 4 sizes.
PLAYERS_FILTER=${PLAYERS_FILTER:-2,4,6,8}

mkdir -p "$OUT"

agent_module_for_race() {
    case "$1" in
        zerg)    echo "python_agent.agents.z_agent_v5" ;;
        terran)  echo "python_agent.agents.t_agent_v5" ;;
        protoss) echo "python_agent.agents.p_agent_v4" ;;
        *)       echo "unknown_race_$1"; return 1 ;;
    esac
}

kill_all() {
    pkill -f openbw_server >/dev/null 2>&1
    pkill -f openbw_observer >/dev/null 2>&1
    pkill -f "agents\." >/dev/null 2>&1
    sleep 1
}

# Pick a random map matching the allowed player counts. Reads from the
# original_resources/ directory each round so if you drop new maps in
# they get picked up.
pick_map() {
    python3 - "$REPO" "$PLAYERS_FILTER" <<'PYEOF'
import os, re, sys, random
repo, plfilter = sys.argv[1], sys.argv[2]
allowed = {int(x) for x in plfilter.split(",") if x.strip().isdigit()}
maps = []
for fn in os.listdir(os.path.join(repo, "original_resources")):
    m = re.match(r"^\((\d+)\)", fn)
    if not m: continue
    n = int(m.group(1))
    if n not in allowed: continue
    if not (fn.lower().endswith(".scm") or fn.lower().endswith(".scx")): continue
    maps.append((n, fn))
if not maps:
    sys.stderr.write("no maps matched filter\n"); sys.exit(1)
n, fn = random.choice(maps)
# Print two lines: <slot count> <path>. Bash reads with read.
print(n)
print(os.path.join(repo, "original_resources", fn))
PYEOF
}

pick_race() {
    local races=(zerg terran protoss)
    echo "${races[$((RANDOM % 3))]}"
}

RESULTS="$OUT/results.tsv"
if [[ ! -f "$RESULTS" ]]; then
    printf "round\tstart_ts\tmap\tplayers\traces\tlaunch_order\tverdict\tworst_obs_real\ttotal_obs_shared\tserver_frames\tnotes\n" > "$RESULTS"
fi

start_ts=$(date +%s)
elapsed() { echo $(($(date +%s) - start_ts)); }

round=0
pass=0
pass_cosmetic=0
fail_real=0
fail_other=0

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

    # Pick map: two lines out (slot count, path).
    mapinfo=$(pick_map) || { echo "map pick failed"; break; }
    N=$(echo "$mapinfo" | sed -n 1p)
    MAP=$(echo "$mapinfo" | sed -n 2p)
    MAP_BASENAME=$(basename "$MAP")

    # Races for slot 0..N-1
    RACES=()
    RACE_ARGS=()
    for i in $(seq 0 $((N-1))); do
        r=$(pick_race)
        RACES+=("$r")
        RACE_ARGS+=(--race "$i=$r")
    done
    RACES_JOINED=$(IFS=,; echo "${RACES[*]}")

    # Build launch list: N agents + N observers, shuffled.
    ORDER=$(python3 -c "
import random
n=$N
xs=[f'A:{i}' for i in range(n)]+[f'O:{i}' for i in range(n)]
random.shuffle(xs)
print(' '.join(xs))")

    echo ""
    echo "==== round $round  map=(${N})$(basename "$MAP" | sed -E 's/^\([0-9]+\)//; s/\.(scm|scx)$//')  races=$RACES_JOINED  elapsed=${now}s ===="
    echo "  round dir: $rdir"
    echo "  launch order: [$ORDER]"

    # Start server.
    ( cd "$REPO" && \
      nohup ./build_srv/server/openbw_server \
        --map "$MAP" --data-path original_resources \
        --users test_resources/users.json \
        "${RACE_ARGS[@]}" \
        --game-speed "$GAME_SPEED" \
        --sync-log "$rdir/server.sync" \
        > "$rdir/server.log" 2>&1 & )

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
        printf "%d\t%s\t%s\t%d\t%s\t[%s]\tSERVER_FAIL\t0\t0\t0\tserver bind timeout\n" \
            "$round" "$(date -u -r $round_start +%FT%TZ)" \
            "$MAP_BASENAME" "$N" "$RACES_JOINED" "$ORDER" \
            >> "$RESULTS"
        fail_other=$((fail_other + 1))
        continue
    fi
    echo "  server ready (obs port $PORT)"

    # Launch entities in shuffled order.
    ord_arr=($ORDER)
    launch_idx=0
    for entity in "${ord_arr[@]}"; do
        launch_idx=$((launch_idx + 1))
        type=${entity%:*}
        slot=${entity##*:}
        key=${KEYS[$slot]}
        name=${NAMES[$slot]}
        race=${RACES[$slot]}

        case "$type" in
            A)
                agent_mod=$(agent_module_for_race "$race")
                ( cd "$REPO" && \
                  PYTHONUNBUFFERED=1 nohup python3 -m "$agent_mod" \
                    "$key" --host 127.0.0.1 --port 6113 --interval-sec 1.0 \
                    > "$rdir/agent_${name}.log" 2>&1 & )
                echo "  [+${launch_idx}] agent $name ($race / slot $slot)"
                ;;
            O)
                ( cd "$REPO" && \
                  nohup ./build_srv/ui/openbw_observer \
                    --map "$MAP" --data-path original_resources \
                    --server 127.0.0.1:$PORT --api-key "$key" \
                    "${RACE_ARGS[@]}" \
                    --sync-log "$rdir/obs_${name}.sync" \
                    > "$rdir/obs_${name}.log" 2>&1 & )
                echo "  [+${launch_idx}] observer $name (slot $slot view)"
                ;;
        esac
        if (( launch_idx < ${#ord_arr[@]} )); then
            sleep "$STAGGER"
        fi
    done

    echo "  playing for ${PLAY_SECS}s..."
    sleep "$PLAY_SECS"

    server_frames=$(grep -oE "frame=[0-9]+" "$rdir/server.log" | tail -1 | cut -d= -f2)
    server_frames=${server_frames:-0}
    echo "  server final frame: $server_frames"

    crash_notes=""
    if grep -q "too much data" "$rdir/server.log" 2>/dev/null; then
        crash_notes="server_msg_buf_overflow"
    elif grep -qE "libc\+\+abi: terminating|exception|abort" "$rdir/server.log" 2>/dev/null; then
        crash_notes="server_crash"
    fi

    kill_all

    # Analyze: compare each observer's sync-log against server across
    # all N slots. Report worst-observer stats plus per-observer detail.
    result=$(python3 <<PYEOF
import os
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

server = load("$rdir/server.sync")
N = $N
names = "${NAMES[*]}".split()

def classify(server, obs):
    min_only=0; real=0; matched=0
    for k, ov in obs.items():
        frame, slot = k
        sv = server.get(k) or server.get((frame+1, slot)) or server.get((frame-1, slot))
        if sv is None: continue
        matched += 1
        if ov == sv: continue
        ou = "\t".join(ov.split("\t")[2:])
        su = "\t".join(sv.split("\t")[2:])
        if ou == su: min_only += 1
        else: real += 1
    return matched, min_only, real

worst_real = 0
total_shared = 0
verdict = "PASS"
details = []
any_shared = False

for slot in range(N):
    name = names[slot]
    obs = load(f"$rdir/obs_{name}.sync")
    sh, mn, rl = classify(server, obs)
    details.append((name, sh, mn, rl))
    total_shared += sh
    if sh > 0: any_shared = True
    if rl > worst_real: worst_real = rl
    if rl > 0:
        verdict = "FAIL_REAL"
    elif mn > 0 and verdict == "PASS":
        verdict = "PASS_COSMETIC"

if not any_shared:
    verdict = "NO_DATA"

detail_str = "; ".join(f"{n}:{s}/{m}/{r}" for n,s,m,r in details)
print(f"verdict={verdict} worst_obs_real={worst_real} total_shared={total_shared} details=[{detail_str}]")
PYEOF
)
    echo "  $result"

    verdict=$(echo "$result" | sed -n 's/.*verdict=\([A-Z_]*\).*/\1/p')
    worst_real=$(echo "$result" | sed -n 's/.*worst_obs_real=\([0-9]*\).*/\1/p')
    total_shared=$(echo "$result" | sed -n 's/.*total_shared=\([0-9]*\).*/\1/p')

    printf "%d\t%s\t%s\t%d\t%s\t[%s]\t%s\t%s\t%s\t%s\t%s\n" \
        "$round" "$(date -u -r $round_start +%FT%TZ)" \
        "$MAP_BASENAME" "$N" "$RACES_JOINED" "$ORDER" \
        "$verdict" "$worst_real" "$total_shared" \
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

    echo "  running tally: PASS=$pass PASS_COSMETIC=$pass_cosmetic FAIL=$fail_real OTHER=$fail_other / $round"
done

echo ""
echo "==== soak summary ===="
echo "  rounds run:           $round"
echo "  passed:               $pass"
echo "  pass_cosmetic only:   $pass_cosmetic"
echo "  fail (real):          $fail_real"
echo "  fail (other):         $fail_other"
echo "  results table:        $RESULTS"
