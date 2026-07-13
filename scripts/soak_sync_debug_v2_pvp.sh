#!/usr/bin/env bash
# Stress-test soak for sync.h observer fan-out (v2 combat soak, P vs P).
#
# Same as scripts/soak_sync_debug_v2.sh but BOTH slots are Protoss
# and BOTH slots run p_agent_debug_v2. Protoss vs Protoss is more
# balanced per-unit than T-vs-P, so the game runs longer before one
# side collapses — more frames = more sync-log coverage.

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"

LOG=/tmp/simsc-logs
mkdir -p "$LOG"
rm -f "$LOG"/*.log

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

# --- 1. server: both slots Protoss ---
nohup ./build_srv/server/openbw_server \
    --data-path original_resources \
    --map "original_resources/(2)Bottleneck.scm" \
    --user 'protoss_a:sk-a:player:0' \
    --user 'protoss_b:sk-b:player:1' \
    --user 'watcher1:sk-w1:observer' \
    --user 'watcher2:sk-w2:observer' \
    --race 0=protoss --race 1=protoss \
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

# --- 2. agents: p_agent_debug_v2 on both slots ---
PYTHONUNBUFFERED=1 nohup python3 -u -m python_agent.agents.p_agent_debug_v2 sk-a \
    --host 127.0.0.1 --port 6113 --interval-sec 0.1 --base-target 2 \
    > "$LOG/agent_A.log" 2>&1 &
A_PID=$!

PYTHONUNBUFFERED=1 nohup python3 -u -m python_agent.agents.p_agent_debug_v2 sk-b \
    --host 127.0.0.1 --port 6113 --interval-sec 0.1 --base-target 2 \
    > "$LOG/agent_B.log" 2>&1 &
B_PID=$!

# --- 3. observers ---
nohup ./build_srv/ui/openbw_observer \
    --data-path original_resources \
    --map "original_resources/(2)Bottleneck.scm" \
    --server 127.0.0.1:6114 \
    --api-key sk-w1 \
    --race 0=protoss --race 1=protoss \
    --sync-log "$LOG/obs_A_sync.log" \
    > "$LOG/obs_A.log" 2>&1 &
OBS_A_PID=$!

nohup ./build_srv/ui/openbw_observer \
    --data-path original_resources \
    --map "original_resources/(2)Bottleneck.scm" \
    --server 127.0.0.1:6114 \
    --api-key sk-w2 \
    --race 0=protoss --race 1=protoss \
    --sync-log "$LOG/obs_B_sync.log" \
    > "$LOG/obs_B.log" 2>&1 &
OBS_B_PID=$!

sleep 5

echo ""
echo "=== all launched ==="
echo "  server:  PID=$SERVER_PID"
echo "  agent A: PID=$A_PID (slot=0, P)"
echo "  agent B: PID=$B_PID (slot=1, P)"
echo "  obs A:   PID=$OBS_A_PID"
echo "  obs B:   PID=$OBS_B_PID"
echo ""
grep -E 'connected|perspective|WARNING' "$LOG/obs_A.log" "$LOG/obs_B.log" 2>&1 | tail -6
echo ""
echo "Ctrl-C to stop."

wait $SERVER_PID
