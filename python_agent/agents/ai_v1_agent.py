"""ai_v1_agent: integrated closed-loop agent.

This is the "real" agent -- one connection, one decision loop, one
intent store. It replaces running miner + trainer + builder + attacker
as four separate processes.

The split demo agents (miner.py, trainer.py, ...) have two problems in
practice:

  1. No shared state. When the miner sends a probe to an assimilator
     and the sim silently rejects the harvest, the miner has no idea;
     the probe sits at the gas building doing nothing. The builder,
     meanwhile, sees "Assimilator in_progress=1" and never retries.
  2. Open loop. Both miner and builder decide based on unit ORDER
     states, which the sim can leave in weird transient values that
     don't map to "actually working". They need to verify from
     observable OUTCOMES (resources rising, buildings appearing).

ai_v1_agent fixes both by:

  - Tracking intents in Python:
      build_intents[type_id] = BuildIntent(worker_id, tile, issued_frame, ...)
      gas_intents[unit_id]   = GasIntent(refinery_id, issued_frame, ...)
  - Verifying each intent every decision tick against the observation
    AND resource-delta history. If a build didn't produce a matching
    unit within a grace window, retry from scratch. If gas isn't
    actually accumulating, un-assign the worker.
  - Deciding at its own cadence (default 1.5s), independent of BW
    frame rate. Between decisions the sim advances ~35 frames.

Usage:
    python3 -m python_agent.agents.ai_v1_agent <api_key>

State on the wire is unchanged -- this is 100% client-side; the server
sees the same 5 verbs it always saw.
"""

from __future__ import annotations

import argparse
import asyncio
from dataclasses import dataclass, field

from python_agent.client import Client
from python_agent.enums import (
    unit_type_name, ORDERS_BY_NAME, UNIT_TYPES_BY_NAME, IDLE_ORDERS,
)
from python_agent.helpers import (
    guess_race, workers, mineral_fields, vespene_geysers,
    nearest, combat_units, own_refineries, own_producers,
    REFINERY_TYPES, PRODUCER_TYPES,
)


# --------------------------------------------------------------------
# Race plans -- what to build in what order, and what to train.
# --------------------------------------------------------------------

@dataclass
class BuildStep:
    kind: str           # "supply" | "gas" | "producer" | "tech"
    type_id: int
    cost_min: int
    max_count: int | None    # None = unlimited (supply)
    anchor: set[int] | None  # unit_types to search near; None = geyser


@dataclass
class TrainPlan:
    worker_producer: int
    worker: int
    worker_cost_min: int
    combat_producer: int
    combat: int
    combat_cost_min: int
    supply_each: int


RACE_BUILD_ORDER: dict[str, list[BuildStep]] = {
    "protoss": [
        BuildStep("supply",   UNIT_TYPES_BY_NAME["Protoss_Pylon"],
                  100, None, {UNIT_TYPES_BY_NAME["Protoss_Nexus"]}),
        BuildStep("gas",      UNIT_TYPES_BY_NAME["Protoss_Assimilator"],
                  100, 1, None),
        BuildStep("producer", UNIT_TYPES_BY_NAME["Protoss_Gateway"],
                  150, 1, {UNIT_TYPES_BY_NAME["Protoss_Pylon"]}),
        BuildStep("tech",     UNIT_TYPES_BY_NAME["Protoss_Cybernetics_Core"],
                  200, 1, {UNIT_TYPES_BY_NAME["Protoss_Pylon"]}),
    ],
    "terran": [
        BuildStep("supply",   UNIT_TYPES_BY_NAME["Terran_Supply_Depot"],
                  100, None, {UNIT_TYPES_BY_NAME["Terran_Command_Center"]}),
        BuildStep("gas",      UNIT_TYPES_BY_NAME["Terran_Refinery"],
                  100, 1, None),
        BuildStep("producer", UNIT_TYPES_BY_NAME["Terran_Barracks"],
                  150, 1, {UNIT_TYPES_BY_NAME["Terran_Command_Center"]}),
    ],
}

RACE_TRAIN: dict[str, TrainPlan] = {
    "protoss": TrainPlan(
        worker_producer=UNIT_TYPES_BY_NAME["Protoss_Nexus"],
        worker=UNIT_TYPES_BY_NAME["Protoss_Probe"],
        worker_cost_min=50,
        combat_producer=UNIT_TYPES_BY_NAME["Protoss_Gateway"],
        combat=UNIT_TYPES_BY_NAME["Protoss_Zealot"],
        combat_cost_min=100,
        supply_each=2,
    ),
    "terran": TrainPlan(
        worker_producer=UNIT_TYPES_BY_NAME["Terran_Command_Center"],
        worker=UNIT_TYPES_BY_NAME["Terran_SCV"],
        worker_cost_min=50,
        combat_producer=UNIT_TYPES_BY_NAME["Terran_Barracks"],
        combat=UNIT_TYPES_BY_NAME["Terran_Marine"],
        combat_cost_min=50,
        supply_each=2,
    ),
}


# --------------------------------------------------------------------
# Intent tracking (client-side only).
# --------------------------------------------------------------------

# BW frame rate at --game-speed fastest is ~24 FPS. A probe-to-Pylon
# walk + construction takes ~700 frames (~30s). Grace = "if we don't
# see either a matching in-progress build or a completed one within
# this many frames, retry."
BUILD_GRACE_FRAMES = 900     # ~ 38s at 24 FPS
GAS_GRACE_FRAMES   = 300     # ~ 12s: enough to walk to geyser + start
GAS_MIN_DELTA_TO_COUNT = 2   # +2 gas over the check window is a heartbeat


@dataclass
class BuildIntent:
    type_id: int
    worker_id: int
    tile_x: int
    tile_y: int
    issued_frame: int


@dataclass
class GasIntent:
    worker_id: int
    refinery_id: int
    issued_frame: int


@dataclass
class AgentState:
    race: str | None = None
    build_plan: list[BuildStep] = field(default_factory=list)
    train_plan: TrainPlan | None = None

    # One in-flight build attempt per type_id at a time.
    build_intents: dict[int, BuildIntent] = field(default_factory=dict)
    # Which workers we have committed to gas.
    gas_intents: dict[int, GasIntent] = field(default_factory=dict)

    # Rolling resource history: [(frame, minerals, gas)] to detect
    # whether gas is actually rising.
    resource_history: list[tuple[int, int, int]] = field(default_factory=list)


# --------------------------------------------------------------------
# Observation helpers.
# --------------------------------------------------------------------

def count_units(units: list[dict], type_id: int) -> tuple[int, int]:
    """(completed, in_progress) for a given unit_type."""
    m = [u for u in units if u["type"] == type_id]
    return (sum(1 for u in m if u.get("completed") is True),
            sum(1 for u in m if not u.get("completed", False)))


def find_unit(units: list[dict], unit_id: int) -> dict | None:
    for u in units:
        if u["unit_id"] == unit_id:
            return u
    return None


def gas_delta(hist: list[tuple[int, int, int]], window_frames: int) -> int | None:
    """How much gas has accrued in the last `window_frames`? None if
    we don't have enough history yet."""
    if not hist:
        return None
    cur_frame, _, cur_gas = hist[-1]
    for f, _, g in reversed(hist):
        if cur_frame - f >= window_frames:
            return cur_gas - g
    return None  # history not deep enough


# --------------------------------------------------------------------
# Decision phases.
# --------------------------------------------------------------------

async def phase_verify_intents(c: Client, s: AgentState, obs: dict) -> None:
    """Prune stale intents. This is the CLOSED-LOOP part."""
    frame = obs["current_frame"]
    units = obs["units"]

    # ---- Build intents ----
    to_drop: list[int] = []
    for type_id, bi in s.build_intents.items():
        completed, in_progress = count_units(units, type_id)
        if completed + in_progress > 0:
            # Something matching is on the field. Assume it's ours;
            # keep the intent as a marker but no need to retry.
            # Once completed > 0, drop the intent so the queue moves on.
            if completed > 0:
                print(f"[ai_v1]   VERIFY: {unit_type_name(type_id)} completed, "
                      f"clearing intent")
                to_drop.append(type_id)
            continue
        # No unit on the field yet. Did we wait too long?
        age = frame - bi.issued_frame
        if age > BUILD_GRACE_FRAMES:
            worker = find_unit(units, bi.worker_id)
            worker_status = ("alive"
                             if worker is not None
                             else "GONE (dead/lost)")
            print(f"[ai_v1]   VERIFY: {unit_type_name(type_id)} intent "
                  f"aged {age} frames, no matching unit, worker={worker_status}. "
                  f"Retrying.")
            to_drop.append(type_id)
    for tid in to_drop:
        s.build_intents.pop(tid, None)

    # ---- Gas intents ----
    gas_to_drop: list[int] = []
    for wid, gi in s.gas_intents.items():
        w = find_unit(units, wid)
        if w is None:
            print(f"[ai_v1]   VERIFY: gas worker {wid} vanished; dropping")
            gas_to_drop.append(wid)
            continue
        # Check the refinery still exists AND is complete. If it's not
        # complete our probe is doing nothing useful.
        r = find_unit(units, gi.refinery_id)
        if r is None or r.get("completed") is not True:
            print(f"[ai_v1]   VERIFY: refinery {gi.refinery_id} not ready; "
                  f"un-assigning worker {wid}")
            gas_to_drop.append(wid)
            continue
        # Order sanity: if the worker fell out of any gas-related order
        # (e.g., back to Guard/Nothing) something rejected the harvest.
        order_name_map = {v: k for k, v in ORDERS_BY_NAME.items()}
        wo = w["order"]
        gas_orders = {ORDERS_BY_NAME["MoveToGas"], ORDERS_BY_NAME["WaitForGas"],
                      ORDERS_BY_NAME["HarvestGas"], ORDERS_BY_NAME["ReturnGas"],
                      ORDERS_BY_NAME["Harvest1"], ORDERS_BY_NAME["Harvest2"]}
        age = frame - gi.issued_frame
        if wo not in gas_orders and age > GAS_GRACE_FRAMES:
            print(f"[ai_v1]   VERIFY: worker {wid} not on any gas order "
                  f"(order={order_name_map.get(wo,'?')}); un-assigning")
            gas_to_drop.append(wid)
    for wid in gas_to_drop:
        s.gas_intents.pop(wid, None)

    # ---- Gas heartbeat: if we THINK we have gas workers but resources.gas
    # isn't going up, something is wrong globally. Un-assign all so we
    # start clean next tick.
    if s.gas_intents:
        dg = gas_delta(s.resource_history, GAS_GRACE_FRAMES)
        if dg is not None and dg < GAS_MIN_DELTA_TO_COUNT:
            print(f"[ai_v1]   VERIFY: {len(s.gas_intents)} gas workers "
                  f"assigned but gas rose by only {dg} in last "
                  f"{GAS_GRACE_FRAMES} frames; wiping gas intents")
            s.gas_intents.clear()


async def phase_build(c: Client, s: AgentState, obs: dict,
                      supply_gap_trigger: int) -> None:
    """Walk the build queue; place the first missing step we can afford."""
    r = obs["resources"]
    units = obs["units"]
    frame = obs["current_frame"]

    for step in s.build_plan:
        # An intent in flight for this step -- wait for verify to
        # either see completion or time it out.
        if step.type_id in s.build_intents:
            return  # one build attempt at a time, keep it simple

        completed, in_progress = count_units(units, step.type_id)

        # Cap: skip if we already have (or are building) enough.
        if step.max_count is not None and completed + in_progress >= step.max_count:
            continue

        if step.kind == "supply":
            gap = r["supply_max"] - r["supply_used"]
            if gap >= supply_gap_trigger:
                continue  # plenty of headroom, skip
            if in_progress > 0:
                return  # one already coming
        else:
            # One-shot: skip if it exists or is coming.
            if completed > 0 or in_progress > 0:
                continue

        # Budget check.
        headroom = 40 if step.kind == "producer" else 20
        if r["minerals"] < step.cost_min + headroom:
            return

        # Pick a free-enough worker (not a gas worker, not already
        # tied to another build).
        busy_workers = set(s.gas_intents.keys()) | {
            bi.worker_id for bi in s.build_intents.values()
        }
        candidates = [u for u in workers(units)
                      if u["unit_id"] not in busy_workers]
        if not candidates:
            print(f"[ai_v1]   BUILD: no free worker for "
                  f"{unit_type_name(step.type_id)}")
            return
        worker = candidates[0]

        # Placement.
        kwargs: dict = {
            "unit_type":    step.type_id,
            "worker_unit":  worker["unit_id"],
            "radius_tiles": 20,
            "max_results":  8,
        }
        if step.kind == "gas":
            geysers = vespene_geysers(obs.get("neutrals", []))
            if not geysers:
                print(f"[ai_v1]   BUILD: no geyser visible for gas step")
                return
            g = geysers[0]
            kwargs["center_x"] = g["x"]
            kwargs["center_y"] = g["y"]
            kwargs["radius_tiles"] = 3

        try:
            resp = await c.find_placement(**kwargs)
        except Exception as e:
            print(f"[ai_v1]   BUILD: find_placement error: {e}")
            return
        spots = resp.get("spots", [])
        if not spots:
            print(f"[ai_v1]   BUILD: no placement for "
                  f"{unit_type_name(step.type_id)}")
            return
        spot = spots[0]

        # Fire.
        try:
            ack = await c.build(unit_id=worker["unit_id"],
                                unit_type=step.type_id,
                                tile_x=spot["tile_x"],
                                tile_y=spot["tile_y"])
        except Exception as e:
            print(f"[ai_v1]   BUILD: cmd error: {e}")
            return
        s.build_intents[step.type_id] = BuildIntent(
            type_id=step.type_id,
            worker_id=worker["unit_id"],
            tile_x=spot["tile_x"],
            tile_y=spot["tile_y"],
            issued_frame=frame,
        )
        print(f"[ai_v1]   BUILD: {unit_type_name(step.type_id)} @ "
              f"tile ({spot['tile_x']},{spot['tile_y']}) "
              f"by worker {worker['unit_id']} frame={frame}")
        return   # one build per tick


async def phase_train(c: Client, s: AgentState, obs: dict,
                      worker_cap: int) -> None:
    """Push train orders to any idle producer."""
    if s.train_plan is None:
        return
    tp = s.train_plan
    units = obs["units"]
    r = obs["resources"]
    n_workers = len(workers(units))

    combat_ready = any(
        u["type"] == tp.combat_producer and u.get("completed") is True
        for u in units
    )

    want_worker = n_workers < worker_cap
    want_combat = combat_ready

    # Try each producer at most once this tick, and update local
    # bookkeeping so we don't double-spend within the tick.
    r_local = {"minerals": r["minerals"],
               "supply_used": r["supply_used"],
               "supply_max": r["supply_max"]}

    def supply_ok() -> bool:
        return r_local["supply_used"] + tp.supply_each <= r_local["supply_max"]

    idle = lambda u: (u["order"] in IDLE_ORDERS and u.get("completed", True))

    if want_worker and r_local["minerals"] >= tp.worker_cost_min and supply_ok():
        for p in units:
            if p["type"] == tp.worker_producer and idle(p):
                try:
                    await c.train(unit_id=p["unit_id"], unit_type=tp.worker)
                    print(f"[ai_v1]   TRAIN: {unit_type_name(tp.worker)} "
                          f"from {p['unit_id']}")
                    r_local["minerals"] -= tp.worker_cost_min
                    r_local["supply_used"] += tp.supply_each
                    p["order"] = -1
                except Exception as e:
                    print(f"[ai_v1]   TRAIN: cmd error: {e}")
                break

    if want_combat and r_local["minerals"] >= tp.combat_cost_min and supply_ok():
        for p in units:
            if p["type"] == tp.combat_producer and idle(p):
                try:
                    await c.train(unit_id=p["unit_id"], unit_type=tp.combat)
                    print(f"[ai_v1]   TRAIN: {unit_type_name(tp.combat)} "
                          f"from {p['unit_id']}")
                    r_local["minerals"] -= tp.combat_cost_min
                    r_local["supply_used"] += tp.supply_each
                    p["order"] = -1
                except Exception as e:
                    print(f"[ai_v1]   TRAIN: cmd error: {e}")
                break


async def phase_mine(c: Client, s: AgentState, obs: dict,
                     target_gas_workers: int) -> None:
    """Assign workers to minerals + gas."""
    units = obs["units"]
    wu = workers(units)
    mfs = mineral_fields(obs.get("neutrals", []))
    refineries = own_refineries(units)

    # ---- 1. Fill gas assignments up to target. ----
    if refineries:
        want_gas = len(refineries) * target_gas_workers
        have_gas = len(s.gas_intents)
        needed = want_gas - have_gas
        if needed > 0:
            # Reserve set: existing gas + build intents.
            reserved = set(s.gas_intents.keys()) | {
                bi.worker_id for bi in s.build_intents.values()
            }
            pool = [u for u in wu if u["unit_id"] not in reserved]
            for w in pool[:needed]:
                target = nearest(w, refineries)
                if target is None:
                    break
                try:
                    await c.gather(unit_id=w["unit_id"],
                                   target_unit=target["unit_id"])
                    s.gas_intents[w["unit_id"]] = GasIntent(
                        worker_id=w["unit_id"],
                        refinery_id=target["unit_id"],
                        issued_frame=obs["current_frame"],
                    )
                    print(f"[ai_v1]   MINE: worker {w['unit_id']} -> "
                          f"gas at refinery {target['unit_id']}")
                except Exception as e:
                    print(f"[ai_v1]   MINE: gas cmd error: {e}")

    # ---- 2. Send genuinely-idle workers to minerals. ----
    if mfs:
        reserved = set(s.gas_intents.keys()) | {
            bi.worker_id for bi in s.build_intents.values()
        }
        for w in wu:
            if w["unit_id"] in reserved:
                continue
            if w["order"] not in IDLE_ORDERS:
                continue
            m = nearest(w, mfs)
            if m is None:
                break
            try:
                await c.gather(unit_id=w["unit_id"], target_unit=m["unit_id"])
                print(f"[ai_v1]   MINE: worker {w['unit_id']} -> "
                      f"mineral {m['unit_id']}")
            except Exception as e:
                print(f"[ai_v1]   MINE: mineral cmd error: {e}")


async def phase_attack(c: Client, s: AgentState, obs: dict,
                       target_x: int, target_y: int) -> None:
    """Attack-move idle combat units toward the enemy corner, or focus
    the nearest visible enemy."""
    cu = combat_units(obs["units"])
    enemies = obs.get("enemies", [])
    for u in cu:
        if u["order"] not in IDLE_ORDERS:
            continue
        if enemies:
            t = nearest(u, enemies)
            if t is None:
                continue
            try:
                await c.attack(unit_id=u["unit_id"], target_unit=t["unit_id"])
                print(f"[ai_v1]   ATTACK: {unit_type_name(u['type'])} "
                      f"{u['unit_id']} -> enemy {t['unit_id']}")
            except Exception as e:
                print(f"[ai_v1]   ATTACK: cmd error: {e}")
        else:
            try:
                await c.attack(unit_id=u["unit_id"], target_unit=0,
                               x=target_x, y=target_y)
                print(f"[ai_v1]   ATTACK: {unit_type_name(u['type'])} "
                      f"{u['unit_id']} -> ({target_x},{target_y})")
            except Exception as e:
                print(f"[ai_v1]   ATTACK: cmd error: {e}")


# --------------------------------------------------------------------
# Main loop.
# --------------------------------------------------------------------

async def run(c: Client, interval_sec: float, worker_cap: int,
              gas_workers: int, supply_gap: int) -> None:
    print(f"[ai_v1] connected as slot={c.welcome.slot} "
          f"at frame={c.welcome.current_frame}")

    # Fetch map dimensions once (for attack fallback).
    map_info = (await c.observe(targets=["map_info"]))["map_info"]
    map_w, map_h = map_info["width"], map_info["height"]

    s = AgentState()
    home_x = home_y = None
    target_x = target_y = None

    while True:
        obs = await c.observe(
            targets=["units", "resources", "enemies", "neutrals"])

        # First-tick initialization.
        if s.race is None:
            s.race = guess_race(obs["units"])
            print(f"[ai_v1] inferred race={s.race}")
            s.build_plan = RACE_BUILD_ORDER.get(s.race, [])
            s.train_plan = RACE_TRAIN.get(s.race)
            if not s.build_plan or s.train_plan is None:
                print(f"[ai_v1] no plan for race={s.race}; exiting")
                return
            if obs["units"]:
                home_x = sum(u["x"] for u in obs["units"]) // len(obs["units"])
                home_y = sum(u["y"] for u in obs["units"]) // len(obs["units"])
                target_x = map_w - home_x
                target_y = map_h - home_y
                print(f"[ai_v1] home~({home_x},{home_y}) "
                      f"attack toward ({target_x},{target_y})")

        # Update resource history for gas-delta checks.
        r = obs["resources"]
        s.resource_history.append(
            (obs["current_frame"], r["minerals"], r["gas"]))
        # Keep the last ~30s worth (way more than any grace window).
        s.resource_history[:] = s.resource_history[-40:]

        # Status line.
        types_summary = []
        for step in s.build_plan:
            c1, c2 = count_units(obs["units"], step.type_id)
            types_summary.append(
                f"{unit_type_name(step.type_id)[:14]}={c1}(+{c2})")
        print(f"[ai_v1] frame={obs['current_frame']} race={s.race} "
              f"min={r['minerals']} gas={r['gas']} "
              f"supply={r['supply_used']}/{r['supply_max']} "
              f"workers={len(workers(obs['units']))} "
              f"combat={len(combat_units(obs['units']))} "
              f"gas_intents={len(s.gas_intents)} "
              f"build_intents={len(s.build_intents)}  |  "
              f"{'  '.join(types_summary)}")

        # 1. Close the loop: verify all outstanding intents.
        await phase_verify_intents(c, s, obs)

        # 2. Assign workers to resources (mine + gas).
        await phase_mine(c, s, obs, gas_workers)

        # 3. Build the next thing in the queue.
        await phase_build(c, s, obs, supply_gap)

        # 4. Train workers + combat units from any idle producer.
        await phase_train(c, s, obs, worker_cap)

        # 5. Send combat units at the enemy.
        if target_x is not None:
            await phase_attack(c, s, obs, target_x, target_y)

        await asyncio.sleep(interval_sec)


async def main(api_key: str, host: str, port: int, interval_sec: float,
               worker_cap: int, gas_workers: int, supply_gap: int) -> None:
    async with Client(api_key=api_key, host=host, port=port) as c:
        await run(c, interval_sec, worker_cap, gas_workers, supply_gap)


def entrypoint() -> None:
    p = argparse.ArgumentParser(
        prog="python3 -m python_agent.agents.ai_v1_agent",
        description="Integrated closed-loop agent (miner+trainer+builder+attacker).")
    p.add_argument("api_key")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=6113)
    p.add_argument("--interval-sec", type=float, default=1.5,
                   help="decision-loop cadence in seconds (default 1.5)")
    p.add_argument("--worker-cap", type=int, default=16)
    p.add_argument("--gas-workers", type=int, default=3)
    p.add_argument("--supply-gap", type=int, default=3)
    args = p.parse_args()
    try:
        asyncio.run(main(args.api_key, args.host, args.port, args.interval_sec,
                         args.worker_cap, args.gas_workers, args.supply_gap))
    except KeyboardInterrupt:
        print("\n[ai_v1] stopped")


if __name__ == "__main__":
    entrypoint()
