#!/bin/bash
# repro_round7.sh -- deterministic reproduction of the FAIL_REAL round
# observed in the 2026-07-11 multi-map soak, round 7.
#
# Config exactly matches the failing round:
#   map      = (6)New Gettysburg.scm
#   races    = protoss, protoss, terran, terran, terran, terran
#   game-speed = 10 (10ms/frame, 100 FPS)
#   play    = 300s
#   stagger = 20s per launch
#   launch order = [O:1 A:0 O:0 O:5 A:2 A:5 O:2 A:4 A:1 O:3 O:4 A:3]
#
# Symptom to reproduce: after 300s of play, observers show INVENTORY
# unit counts that differ from server AND from each other. Bob (first
# joiner) diverges from server; the 5 late-joiners agree with each
# other on a third state.

set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
OUT=${OUT:-/tmp/repro_r7}

KEYS=(
    sk-LYvXIzRDaDEe8GTlPgyf8eMxAyamUJt_Ig2413DbEjw   # alice slot 0
    sk-anYTfuY-QL9szAzIlvtv44RxpgJlJPC1ocqIA26qpf0   # bob   slot 1
    sk-jdDXKI5p67vcaxRx9MTGs6H0qnAm5pGmi270C3uJUfY   # carol slot 2
    sk-8tvfxEmYOmBPXi6ei_FQrwFmLqfm-gkMbVBpchBxOqo   # dave  slot 3
    sk-8_RWcJuffoj0cICWlN1kjUZtCa7G4j1yR4bs_8sWTIM   # eve   slot 4
    sk-MJyi6EsR2ZraWvk6Lp-N7V4qbqeFG_ymOzDcetA1i7g   # frank slot 5
)
NAMES=(alice bob carol dave eve frank)
RACES=(protoss protoss terran terran terran terran)
AGENT_MODS=(
    python_agent.agents.p_agent_v4
    python_agent.agents.p_agent_v4
    python_agent.agents.t_agent_v5
    python_agent.agents.t_agent_v5
    python_agent.agents.t_agent_v5
    python_agent.agents.t_agent_v5
)

ORDER=(O:1 A:0 O:0 O:5 A:2 A:5 O:2 A:4 A:1 O:3 O:4 A:3)

PLAY_SECS=${PLAY_SECS:-300}
STAGGER=${STAGGER:-20}
PORT=${PORT:-6114}
MAP="$REPO/original_resources/(6)New Gettysburg.scm"

mkdir -p "$OUT"
rm -f "$OUT"/*.log "$OUT"/*.sync

kill_all() {
    pkill -f openbw_server >/dev/null 2>&1
    pkill -f openbw_observer >/dev/null 2>&1
    pkill -f "agents\." >/dev/null 2>&1
    sleep 1
}
kill_all

RACE_ARGS=()
for i in 0 1 2 3 4 5; do
    RACE_ARGS+=(--race "$i=${RACES[$i]}")
done

echo "[repro-r7] starting server (map=New Gettysburg, races=${RACES[*]}, speed=10)"
( cd "$REPO" && \
  nohup ./build_srv/server/openbw_server \
    --map "$MAP" --data-path original_resources \
    --users test_resources/users.json \
    "${RACE_ARGS[@]}" \
    --game-speed 10 \
    --sync-log "$OUT/server.sync" \
    > "$OUT/server.log" 2>&1 & )

for i in $(seq 1 60); do
    if nc -z 127.0.0.1 6113 2>/dev/null && nc -z 127.0.0.1 $PORT 2>/dev/null; then
        break
    fi
    sleep 0.25
done
echo "[repro-r7] server ready"

idx=0
for entity in "${ORDER[@]}"; do
    idx=$((idx + 1))
    type=${entity%:*}
    slot=${entity##*:}
    key=${KEYS[$slot]}
    name=${NAMES[$slot]}
    race=${RACES[$slot]}

    case "$type" in
        A)
            mod=${AGENT_MODS[$slot]}
            ( cd "$REPO" && \
              PYTHONUNBUFFERED=1 nohup python3 -m "$mod" \
                "$key" --host 127.0.0.1 --port 6113 --interval-sec 1.0 \
                > "$OUT/agent_${name}.log" 2>&1 & )
            echo "[repro-r7] [+${idx}] agent $name ($race / slot $slot)"
            ;;
        O)
            ( cd "$REPO" && \
              nohup ./build_srv/ui/openbw_observer \
                --map "$MAP" --data-path original_resources \
                --server 127.0.0.1:$PORT --api-key "$key" \
                "${RACE_ARGS[@]}" \
                --sync-log "$OUT/obs_${name}.sync" \
                > "$OUT/obs_${name}.log" 2>&1 & )
            echo "[repro-r7] [+${idx}] observer $name (slot $slot view)"
            ;;
    esac
    if (( idx < ${#ORDER[@]} )); then
        sleep "$STAGGER"
    fi
done

echo "[repro-r7] all launched. Playing ${PLAY_SECS}s..."
sleep "$PLAY_SECS"

echo "[repro-r7] final server frame: $(grep -oE "frame=[0-9]+" "$OUT/server.log" | tail -1)"
echo "[repro-r7] stopping all processes"
kill_all

echo ""
echo "[repro-r7] --- results ---"
python3 <<PYEOF
import os
def load(p):
    out = {}
    if not os.path.exists(p): return out
    for ln in open(p):
        if "INVENTORY" not in ln: continue
        parts = ln.rstrip().split("\t")
        try:
            f = int(parts[1]); s = int(parts[3].split("=")[1])
            out[(f,s)] = "\t".join(parts[4:])
        except: pass
    return out

srv = load("$OUT/server.sync")
names = "alice bob carol dave eve frank".split()
for name in names:
    obs = load(f"$OUT/obs_{name}.sync")
    matched = min_only = real = 0
    for k, ov in obs.items():
        f, s = k
        sv = srv.get(k) or srv.get((f+1, s)) or srv.get((f-1, s))
        if sv is None: continue
        matched += 1
        if ov == sv: continue
        ou = "\t".join(ov.split("\t")[2:])
        su = "\t".join(sv.split("\t")[2:])
        if ou == su: min_only += 1
        else: real += 1
    tag = "PASS" if real == 0 and min_only == 0 else ("COSMETIC" if real == 0 else "FAIL_REAL")
    print(f"  {name:6s}  matched={matched:4d}  min_only={min_only:3d}  real={real:3d}  {tag}")
PYEOF
echo ""
echo "logs at $OUT/"
