"""Attacker agent: attack-move every combat unit toward the presumed
enemy start location.

Doesn't know where the enemy actually is (no scouting), so it just
attack-moves toward the opposite corner of the map. When enemies come
into view we focus on the nearest visible one instead.

Best paired with a trainer agent (attacker.py alone won't have combat
units to send unless the map spawns them).

Usage:
    python3 -m python_agent.agents.attacker <api_key>
"""

from __future__ import annotations

import argparse
import asyncio

from python_agent.client import Client
from python_agent.enums import unit_type_name, order_name, IDLE_ORDERS
from python_agent.helpers import combat_units, nearest


async def run(c: Client, interval_sec: float) -> None:
    print(f"[attacker] connected as slot={c.welcome.slot} "
          f"at frame={c.welcome.current_frame}")

    # Fetch map dimensions once.
    map_info = (await c.observe(targets=["map_info"]))["map_info"]
    map_w, map_h = map_info["width"], map_info["height"]

    # Sample our starting position to guess which corner is "away".
    obs = await c.observe(targets=["units"])
    if not obs["units"]:
        print("[attacker] no starting units; exiting")
        return
    home_x = sum(u["x"] for u in obs["units"]) // len(obs["units"])
    home_y = sum(u["y"] for u in obs["units"]) // len(obs["units"])
    # Push toward the opposite corner.
    target_x = map_w - home_x
    target_y = map_h - home_y
    print(f"[attacker] home~({home_x},{home_y}) attack toward ({target_x},{target_y})")

    while True:
        obs = await c.observe(targets=["units", "enemies", "resources"])
        cu = combat_units(obs["units"])
        enemies = obs.get("enemies", [])

        print(f"[attacker] frame={obs['current_frame']} "
              f"combat_units={len(cu)} visible_enemies={len(enemies)}")

        # Priority: any visible enemy? focus-fire the nearest.
        for u in cu:
            if u["order"] not in IDLE_ORDERS and enemies:
                # Only redirect idle units unless we have a hot target.
                continue
            if enemies:
                target = nearest(u, enemies)
                if target is None:
                    continue
                try:
                    ack = await c.attack(unit_id=u["unit_id"],
                                         target_unit=target["unit_id"])
                    print(f"[attacker]  {unit_type_name(u['type'])} "
                          f"{u['unit_id']} -> attack enemy {target['unit_id']} "
                          f"@frame={ack['queued_at_frame']}")
                except Exception as e:
                    print(f"[attacker]  cmd error: {e}")
            elif u["order"] in IDLE_ORDERS:
                # No enemy in view, unit is idle -- march toward assumed home.
                try:
                    ack = await c.attack(unit_id=u["unit_id"], target_unit=0,
                                         x=target_x, y=target_y)
                    print(f"[attacker]  {unit_type_name(u['type'])} "
                          f"{u['unit_id']} -> attack-move ({target_x},{target_y}) "
                          f"@frame={ack['queued_at_frame']}")
                except Exception as e:
                    print(f"[attacker]  cmd error: {e}")

        await asyncio.sleep(interval_sec)


async def main(api_key: str, host: str, port: int, interval_sec: float) -> None:
    async with Client(api_key=api_key, host=host, port=port) as c:
        await run(c, interval_sec)


def entrypoint() -> None:
    p = argparse.ArgumentParser(prog="python3 -m python_agent.agents.attacker")
    p.add_argument("api_key")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=6113)
    p.add_argument("--interval-sec", type=float, default=3.0)
    args = p.parse_args()
    try:
        asyncio.run(main(args.api_key, args.host, args.port, args.interval_sec))
    except KeyboardInterrupt:
        print("\n[attacker] stopped")


if __name__ == "__main__":
    entrypoint()
