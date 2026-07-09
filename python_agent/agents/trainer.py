"""Trainer agent: train workers from the main base + combat units from
producer buildings.

Per race:

              main producer  worker    combat producer   combat unit
  Terran:     Command_Center SCV       Barracks          Marine
  Protoss:    Nexus          Probe     Gateway           Zealot
  Zerg:       Larva          Drone     Larva             Zergling

For Zerg the Larva is both the worker-morph source and the combat-morph
source, so the same producer type appears in both slots. When a Larva
is idle and we have combat units enabled, we alternate.

Worker cap: default 16. Past that we stop building workers so combat
production isn't starved.

Usage:
    python3 -m python_agent.agents.trainer <api_key> [--worker-cap N]
"""

from __future__ import annotations

import argparse
import asyncio

from python_agent.client import Client
from python_agent.enums import (
    unit_type_name,
    ORDERS_BY_NAME, UNIT_TYPES_BY_NAME,
)
from python_agent.helpers import guess_race, workers


# race -> plan. Each entry describes both worker + combat production.
RACE_PRODUCERS = {
    "terran": {
        "worker_producer": UNIT_TYPES_BY_NAME["Terran_Command_Center"],
        "worker":          UNIT_TYPES_BY_NAME["Terran_SCV"],
        "worker_cost_min": 50,
        "combat_producer": UNIT_TYPES_BY_NAME["Terran_Barracks"],
        "combat":          UNIT_TYPES_BY_NAME["Terran_Marine"],
        "combat_cost_min": 50,
        "supply_each":     2,
    },
    "protoss": {
        "worker_producer": UNIT_TYPES_BY_NAME["Protoss_Nexus"],
        "worker":          UNIT_TYPES_BY_NAME["Protoss_Probe"],
        "worker_cost_min": 50,
        "combat_producer": UNIT_TYPES_BY_NAME["Protoss_Gateway"],
        "combat":          UNIT_TYPES_BY_NAME["Protoss_Zealot"],
        "combat_cost_min": 100,
        "supply_each":     2,
    },
    "zerg": {
        # Zerg reuses Larva for both. combat_producer==worker_producer.
        "worker_producer": UNIT_TYPES_BY_NAME["Zerg_Larva"],
        "worker":          UNIT_TYPES_BY_NAME["Zerg_Drone"],
        "worker_cost_min": 50,
        "combat_producer": UNIT_TYPES_BY_NAME["Zerg_Larva"],
        "combat":          UNIT_TYPES_BY_NAME["Zerg_Zergling"],
        "combat_cost_min": 50,
        "supply_each":     1,   # Zerglings come in pairs
    },
}


# A producer is "idle" (i.e. not currently training) when its order is
# a plain guard state. Buildings training a unit sit in Train orders
# with train_queue > 0.
IDLE_ORDERS: set[int] = {
    ORDERS_BY_NAME["Guard"],
    ORDERS_BY_NAME["PlayerGuard"],
    ORDERS_BY_NAME["Nothing"],
}


def _find_idle_producer(units: list[dict], type_id: int) -> dict | None:
    for u in units:
        if (u["type"] == type_id
                and u["order"] in IDLE_ORDERS
                and u.get("completed", True)):
            return u
    return None


async def run(c: Client, interval_sec: float, worker_cap: int) -> None:
    print(f"[trainer] connected as slot={c.welcome.slot} "
          f"at frame={c.welcome.current_frame}")

    race = None
    plan: dict | None = None
    prefer_combat = False  # for Zerg-style alternation on shared producer

    while True:
        obs = await c.observe(targets=["units", "resources"])

        if race is None:
            race = guess_race(obs["units"])
            plan = RACE_PRODUCERS.get(race)
            print(f"[trainer] inferred race={race}")
            if plan is None:
                print(f"[trainer] no plan for race={race}; exiting")
                return

        r = obs["resources"]
        n_workers = len(workers(obs["units"]))

        # Combat production unlocked once we have a completed producer.
        combat_ready = any(
            u["type"] == plan["combat_producer"] and u.get("completed") is True
            for u in obs["units"]
        )

        want_worker = n_workers < worker_cap
        want_combat = combat_ready

        print(f"[trainer] frame={obs['current_frame']} race={race} "
              f"min={r['minerals']} supply={r['supply_used']}/{r['supply_max']} "
              f"workers={n_workers}/{worker_cap} combat_ready={combat_ready}")

        supply_ok = (r["supply_used"] + plan["supply_each"] <= r["supply_max"])

        # Pick which order(s) to try this tick. When both are wanted and
        # producers are the same type (Zerg Larva), alternate so neither
        # side starves.
        shared = plan["worker_producer"] == plan["combat_producer"]
        if shared and want_worker and want_combat:
            order = ["combat", "worker"] if prefer_combat else ["worker", "combat"]
        elif want_worker and want_combat:
            order = ["worker", "combat"]
        elif want_worker:
            order = ["worker"]
        elif want_combat:
            order = ["combat"]
        else:
            order = []

        for kind in order:
            if kind == "worker":
                cost = plan["worker_cost_min"]
                unit_type_id = plan["worker"]
                producer_type = plan["worker_producer"]
            else:
                cost = plan["combat_cost_min"]
                unit_type_id = plan["combat"]
                producer_type = plan["combat_producer"]

            if r["minerals"] < cost or not supply_ok:
                continue
            p = _find_idle_producer(obs["units"], producer_type)
            if p is None:
                continue
            try:
                ack = await c.train(unit_id=p["unit_id"],
                                    unit_type=unit_type_id)
                print(f"[trainer]  {unit_type_name(p['type'])} "
                      f"{p['unit_id']} -> train {unit_type_name(unit_type_id)} "
                      f"@frame={ack['queued_at_frame']}")
                # Local bookkeeping so we don't re-pick this producer or
                # over-spend within this tick.
                p["order"] = -1
                r["minerals"] -= cost
                r["supply_used"] += plan["supply_each"]
                prefer_combat = (kind == "worker")
            except Exception as e:
                print(f"[trainer]  cmd error: {e}")

        await asyncio.sleep(interval_sec)


async def main(api_key: str, host: str, port: int,
               interval_sec: float, worker_cap: int) -> None:
    async with Client(api_key=api_key, host=host, port=port) as c:
        await run(c, interval_sec, worker_cap)


def entrypoint() -> None:
    p = argparse.ArgumentParser(prog="python3 -m python_agent.agents.trainer")
    p.add_argument("api_key")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=6113)
    p.add_argument("--interval-sec", type=float, default=2.0)
    p.add_argument("--worker-cap", type=int, default=16,
                   help="soft cap on worker count before we shift to combat")
    args = p.parse_args()
    try:
        asyncio.run(main(args.api_key, args.host, args.port,
                         args.interval_sec, args.worker_cap))
    except KeyboardInterrupt:
        print("\n[trainer] stopped")


if __name__ == "__main__":
    entrypoint()
