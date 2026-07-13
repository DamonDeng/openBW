#!/usr/bin/env bash
# Stress-test soak for sync.h observer fan-out.
#
# Launches:
#   - openbw_server at speed=10 with 2 human-role slots (T=0, P=1)
#     + one observer-role user for the SDL observers.
#   - t_agent_debug_v1 on slot 0 (Terran).
#   - p_agent_debug_v1 on slot 1 (Protoss).
#   - Two native SDL observers, both using the observer-role key
#     (which yields perspective=-1 = full map, no fog).
#
# Both debug agents are configured to spam production/build actions
# and never issue attack/move/lift/repair/mine. Combat units stack
# at their spawn point; buildings stay clustered near home. Any
# visible mismatch between the two observer windows is a real sync
# bug — no "did that unit move" ambiguity.
#
# Compare the resulting sync-logs to find the first frame where an
# observer diverges from the server:
#
#   scripts/compare_sync_logs.sh   # if it exists; otherwise use the
#                                   # awk one-liner in the plan file.
#
# Exit any time with Ctrl-C — the script traps and cleans up.

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"

LOG=/tmp/simsc-logs
mkdir -p "$LOG"
rm -f "$LOG"/*.log

# --- kill any previous stragglers ---
pkill -f openbw_server 2>/dev/null || true
pkill -f openbw_observer 2>/dev/null || true
pkill -f 'python_agent.agents' 2>/dev/null || true
sleep 1

cleanup() {
    echo ""
    echo "=== cleaning up ==="
    pkill -f openbw_server 2>/dev/null || true
    pkill -f openbw_observer 2>/dev/null || true
    pkill -f 'python_agent.agents' 2>/dev/null || true
    echo "logs remain in $LOG/"
    ls -lS "$LOG"/*.log 2>/dev/null | awk '{print $5, $NF}'
}
trap cleanup EXIT INT TERM

# --- 1. server ---
nohup ./build_srv/server/openbw_server \
    --data-path original_resources \
    --map "original_resources/(2)Bottleneck.scm" \
    --user 'terran_p:sk-t:player:0' \
    --user 'protoss_p:sk-p:player:1' \
    --user 'watcher1:sk-w1:observer' \
    --user 'watcher2:sk-w2:observer' \
    --race 0=terran --race 1=protoss \
    --game-speed 10 \
    --sync-log "$LOG/server_sync.log" \
    > "$LOG/server.log" 2>&1 &
SERVER_PID=$!
echo "server PID=$SERVER_PID"
sleep 3
if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo "server died at startup!"
    tail -20 "$LOG/server.log"
    exit 1
fi

# --- 2. agents ---
PYTHONUNBUFFERED=1 nohup python3 -u -m python_agent.agents.t_agent_debug_v1 sk-t \
    --host 127.0.0.1 --port 6113 --interval-sec 0.1 --base-target 2 \
    > "$LOG/agent_terran.log" 2>&1 &
T_AGENT_PID=$!

PYTHONUNBUFFERED=1 nohup python3 -u -m python_agent.agents.p_agent_debug_v1 sk-p \
    --host 127.0.0.1 --port 6113 --interval-sec 0.1 --base-target 2 \
    > "$LOG/agent_protoss.log" 2>&1 &
P_AGENT_PID=$!

# --- 3. observers ---
nohup ./build_srv/ui/openbw_observer \
    --data-path original_resources \
    --map "original_resources/(2)Bottleneck.scm" \
    --server 127.0.0.1:6114 \
    --api-key sk-w1 \
    --race 0=terran --race 1=protoss \
    --sync-log "$LOG/obs_A_sync.log" \
    > "$LOG/obs_A.log" 2>&1 &
OBS_A_PID=$!

nohup ./build_srv/ui/openbw_observer \
    --data-path original_resources \
    --map "original_resources/(2)Bottleneck.scm" \
    --server 127.0.0.1:6114 \
    --api-key sk-w2 \
    --race 0=terran --race 1=protoss \
    --sync-log "$LOG/obs_B_sync.log" \
    > "$LOG/obs_B.log" 2>&1 &
OBS_B_PID=$!

sleep 5

echo ""
echo "=== all launched ==="
echo "  server:       PID=$SERVER_PID  log=$LOG/server.log"
echo "  agent Terran: PID=$T_AGENT_PID log=$LOG/agent_terran.log"
echo "  agent Protoss:PID=$P_AGENT_PID log=$LOG/agent_protoss.log"
echo "  obs A:        PID=$OBS_A_PID   log=$LOG/obs_A.log"
echo "  obs B:        PID=$OBS_B_PID   log=$LOG/obs_B.log"
echo ""
echo "sync-logs will land in:"
echo "  server: $LOG/server_sync.log"
echo "  obs A : $LOG/obs_A_sync.log"
echo "  obs B : $LOG/obs_B_sync.log"
echo ""
echo "connection status:"
grep -E 'connected|perspective|WARNING' "$LOG/obs_A.log" "$LOG/obs_B.log" 2>&1 | tail -6
echo ""
echo "Ctrl-C to stop. Otherwise run for 15+ minutes to build up a"
echo "meaningful sync sample."

# Idle here until Ctrl-C or a process dies.
wait $SERVER_PID
