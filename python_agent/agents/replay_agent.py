"""
replay_agent.py — a deterministic replay agent.

Parses a captured AGENT_ISSUE stream from a server sync-log and re-issues
each recorded action at the same `queued_at_frame`, in the same order,
for the same slot. Used together with `--fixed-initial-rand` to make a
soak-test round bit-reproducible: same seed + same action stream = same
sim state on the server every run.

Usage:
    python3 -m python_agent.agents.replay_agent <api_key> --slot N \\
        --sync-log <server.sync path> \\
        [--host 127.0.0.1] [--port 6113] [--interval-sec 0.05]

Each server sync-log line looks like:
    AGENT_ISSUE\trid=...\talias=...\tslot=N\tqueued_at_frame=F\tverb=V\tpayload={...}

We filter to entries matching --slot, sort by queued_at_frame + line order,
then when the game's current_frame reaches F we send the payload as a cmd.
The observed-frame comes from the standard `observe` request the agent WS
supports; we poll it every --interval-sec and drain any actions whose
queued_at_frame <= current_frame.

Timing note: we don't try to hit the SAME wall-clock; the goal is
same-sim-frame determinism. If our observe loop lags, actions bunch up
into a burst and fire together as soon as the frame catches up. That
matches the original run's within-frame ordering (server order-preserves
actions arriving in the same frame).
"""

import argparse
import asyncio
import json
import re
import sys
from pathlib import Path
from python_agent import client as agent_client


AGENT_ISSUE_RE = re.compile(
    r"AGENT_ISSUE\trid=(?P<rid>[0-9a-f]+)\t"
    r"alias=(?P<alias>[^\t]+)\t"
    r"slot=(?P<slot>\-?\d+)\t"
    r"queued_at_frame=(?P<frame>\d+)\t"
    r"verb=(?P<verb>[^\t]+)\t"
    r"payload=(?P<payload>\{.*\})$"
)


def load_actions(sync_log_path: str, slot: int):
    """Return list of (queued_at_frame, payload_dict, rid) sorted by frame + line order.

    Falls back gracefully on truncated logs.
    """
    out = []
    with open(sync_log_path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if not line.startswith("AGENT_ISSUE\t"):
                continue
            m = AGENT_ISSUE_RE.match(line.rstrip("\n"))
            if not m:
                continue
            if int(m.group("slot")) != slot:
                continue
            try:
                payload = json.loads(m.group("payload"))
            except json.JSONDecodeError:
                continue
            out.append((int(m.group("frame")), payload, m.group("rid")))
    # Preserve original in-frame ordering (list index within same frame).
    return out


async def run(api_key: str, host: str, port: int, sync_log: str,
              slot: int, interval_sec: float):
    print(f"[replay] loading actions from {sync_log} for slot={slot}",
          flush=True)
    actions = load_actions(sync_log, slot)
    if not actions:
        print(f"[replay] no AGENT_ISSUE entries found for slot={slot}; "
              "check --slot matches server.sync log",
              file=sys.stderr, flush=True)
        return 2
    print(f"[replay] loaded {len(actions)} actions, "
          f"frame range {actions[0][0]}..{actions[-1][0]}",
          flush=True)

    async with agent_client.Client(api_key, host=host, port=port) as c:
        print(f"[replay] connected slot={c.welcome.slot} "
              f"start_frame={c.welcome.current_frame}",
              flush=True)
        if c.welcome.slot != slot:
            print(f"[replay] WARN: connected slot ({c.welcome.slot}) "
                  f"doesn't match --slot ({slot}); "
                  f"you likely wired the wrong API key. Continuing anyway.",
                  file=sys.stderr, flush=True)

        idx = 0
        n_sent = 0
        n_late = 0    # actions we tried to fire but the sim had already
                      # passed their target frame -- fire immediately.
        while idx < len(actions):
            obs = await c.observe()
            cf = int(obs["current_frame"])
            # Drain all actions whose frame <= current sim frame. If cf
            # is well past a frame, the action fires late; that skews the
            # replay but keeps determinism as close as we can. We can't
            # send actions "backwards in time" so anything we missed
            # queues up and fires next tick.
            burst = 0
            first_frame_this_burst = None
            while idx < len(actions) and actions[idx][0] <= cf:
                frame, payload, rid = actions[idx]
                if first_frame_this_burst is None:
                    first_frame_this_burst = frame
                idx += 1
                try:
                    await c.cmd(payload)
                    n_sent += 1
                    burst += 1
                    if cf > frame + 2:  # 2 tick tolerance
                        n_late += 1
                except agent_client.AgentError as e:
                    # Replay is best-effort — a rejected action just gets
                    # dropped. The interesting bug is in the sync layer,
                    # not the sim's action validation.
                    print(f"[replay] action rid={rid} verb={payload.get('verb')} "
                          f"rejected: {e}", file=sys.stderr, flush=True)
            if burst:
                lag = cf - first_frame_this_burst if first_frame_this_burst else 0
                print(f"[replay] cf={cf} sent {burst} actions "
                      f"(first@f={first_frame_this_burst} lag={lag}f, "
                      f"{n_sent}/{len(actions)} total, {n_late} late; "
                      f"next at "
                      f"{actions[idx][0] if idx < len(actions) else 'END'})",
                      flush=True)
            await asyncio.sleep(interval_sec)

        # Do a final observe so any last-tick actions have a chance to
        # settle before we drop the connection.
        await c.observe()
        print(f"[replay] finished: {n_sent} actions sent", flush=True)
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("api_key")
    p.add_argument("--slot", type=int, required=True,
                   help="which slot's AGENT_ISSUE lines to replay")
    p.add_argument("--sync-log", required=True,
                   help="path to a server.sync file containing AGENT_ISSUE lines")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=6113)
    p.add_argument("--interval-sec", type=float, default=0.05,
                   help="observe loop cadence; smaller = finer-grained "
                        "frame targeting (default 0.05s)")
    args = p.parse_args()
    rc = asyncio.run(run(args.api_key, args.host, args.port,
                         args.sync_log, args.slot, args.interval_sec))
    sys.exit(rc)


if __name__ == "__main__":
    main()
