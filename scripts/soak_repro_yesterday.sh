#!/usr/bin/env bash
# Reproduce yesterday's diverging soak (2026-07-12).
#
# This script is a POINT-IN-TIME copy of the exact config that
# produced the Terran-observer divergence at frame 24862 during the
# 2026-07-12 speed=10 T-vs-P soak. Every other variable is either
# identical to that run or is called out below with a WHY.
#
# Full config:
#   * openbw_server on the current build (build_srv/server/openbw_server)
#   * Map: original_resources/(2)Bottleneck.scm
#   * Race: slot 0 = terran, slot 1 = protoss
#   * --game-speed 10 (100 FPS internal, 10 ms/frame)
#   * Two player accounts:
#         terran_p / sk-t / role=player / slot 0
#         protoss_p / sk-p / role=player / slot 1
#     No separate observer account (see below).
#   * Terran agent: python_agent.agents.t_agent_v5 (full v5, not debug)
#         - Default --interval-sec 1.5
#         - Default --base-target 4
#         - Default scouting on (--scout-radial 1 --scout-zscan 1)
#         - All Terran-tactical passes on (lift, repair, siege,
#           mine, coverage)
#   * Protoss agent: python_agent.agents.p_agent_v4 (full v4)
#         - Same defaults as above (interval 1.5, base 4, scouts on)
#   * Two native SDL observers, EACH authenticating with a PLAYER
#     KEY (sk-t and sk-p) — this means each observer inherits the
#     PLAYER role and the server's perspective_for hook returns
#     that player's assigned_slot, giving each observer per-slot
#     fog-of-war. This is a distinct code path from
#     role=observer/slot=-1 (no-fog) tested today.
#
# WHAT WE'RE TESTING:
# Yesterday this exact setup produced a Terran-observer state-hash
# divergence at frame 24862 while the Protoss observer stayed clean.
# The purpose of this rerun is:
#   1. Confirm the divergence is reproducible given the same config.
#   2. If yes → drive the bisection: which of the many differences
#      vs today's clean debug-agent soaks is responsible?
#         - full-agent vs debug-agent (many verbs)
#         - interval 1.5 vs 0.1
#         - player-role observers (fog) vs observer-role (no-fog)
#         - base-target 4 (buildings spread across map)
#
# Note: yesterday's actual invocation used the same log directory
# (/tmp/simsc-logs), and yesterday's logs were overwritten by
# today's debug soaks. So we're reconstructing from the transcript,
# not re-running the exact log files.

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

# --- 1. server: NO watcher accounts, just the two players ---
nohup ./build_srv/server/openbw_server \
    --data-path original_resources \
    --map "original_resources/(2)Bottleneck.scm" \
    --user 'terran_p:sk-t:player:0' \
    --user 'protoss_p:sk-p:player:1' \
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

# --- 2. agents: FULL v5 / v4, default interval 1.5, default base 4 ---
PYTHONUNBUFFERED=1 nohup python3 -u -m python_agent.agents.t_agent_v5 sk-t \
    --host 127.0.0.1 --port 6113 \
    > "$LOG/agent_terran.log" 2>&1 &
T_AGENT_PID=$!

PYTHONUNBUFFERED=1 nohup python3 -u -m python_agent.agents.p_agent_v4 sk-p \
    --host 127.0.0.1 --port 6113 \
    > "$LOG/agent_protoss.log" 2>&1 &
P_AGENT_PID=$!

# --- 3. observers: each authenticates with a PLAYER key ---
# Terran observer uses the Terran player's key (sk-t) → sees from
# slot 0's perspective (with fog).
nohup ./build_srv/ui/openbw_observer \
    --data-path original_resources \
    --map "original_resources/(2)Bottleneck.scm" \
    --server 127.0.0.1:6114 \
    --api-key sk-t \
    --race 0=terran --race 1=protoss \
    --sync-log "$LOG/obs_terran_sync.log" \
    > "$LOG/obs_terran.log" 2>&1 &
OBS_T_PID=$!

# Protoss observer uses sk-p → slot 1 perspective (with fog).
nohup ./build_srv/ui/openbw_observer \
    --data-path original_resources \
    --map "original_resources/(2)Bottleneck.scm" \
    --server 127.0.0.1:6114 \
    --api-key sk-p \
    --race 0=terran --race 1=protoss \
    --sync-log "$LOG/obs_protoss_sync.log" \
    > "$LOG/obs_protoss.log" 2>&1 &
OBS_P_PID=$!

sleep 5

echo ""
echo "=== all launched ==="
echo "  server:       PID=$SERVER_PID"
echo "  agent Terran: PID=$T_AGENT_PID  (t_agent_v5 full)"
echo "  agent Protoss:PID=$P_AGENT_PID  (p_agent_v4 full)"
echo "  obs T (sk-t): PID=$OBS_T_PID    (player role, slot 0 fog)"
echo "  obs P (sk-p): PID=$OBS_P_PID    (player role, slot 1 fog)"
echo ""
echo "sync-logs:"
echo "  server:   $LOG/server_sync.log"
echo "  obs T:    $LOG/obs_terran_sync.log"
echo "  obs P:    $LOG/obs_protoss_sync.log"
echo ""
grep -E 'connected|perspective|WARNING' "$LOG/obs_terran.log" "$LOG/obs_protoss.log" 2>&1 | tail -6
echo ""
echo "Expected outcome: yesterday's soak diverged around frame 24862"
echo "on the Terran observer. Let this run at least until game frame"
echo "30k+ (~5-7 min real time at speed=10) to give the divergence"
echo "point time to materialise."
echo ""
echo "Ctrl-C to stop."

wait $SERVER_PID
