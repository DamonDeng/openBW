"""ai_v2_agent: exploratory, coverage-oriented agent.

The point of this agent is NOT to play well. It's to hit every
train/build verb combination for a given race so we can:

  1. Confirm the API round-trip for each unit/building type.
  2. Discover which commands the sim silently drops (and why) by
     comparing our command stream against the observation deltas.
  3. Cover placement, resource, prereq, and supply edge cases without
     hand-writing a build order.

Strategy: dumb greedy loop. Every tick, walk a decision list:

  - Do we have >= worker_target probes? no -> train probe
  - Is supply_gap < 8?                     no -> build a Pylon (spread)
  - Any Assimilator?                       no -> build Assimilator on geyser
  - Do we have one of building X?          no -> try build X
  - Do we have one of unit Y from producer Z (Z is complete)?
                                            no -> train Y

Each "try" fires a command optimistically. We don't check prereqs
client-side -- if the sim silently rejects, we detect that by
comparing the next observation's count / resource-delta and log the
reject. Rate-limited: every 5th failure per (verb, target).

Attack: any completed non-worker non-building is attack-moved toward
the opposite corner. Also exercises the `move` and `stop` verbs once
each per session, purely for API coverage.

Usage:
    python3 -m python_agent.agents.ai_v2_agent <api_key>
"""

from __future__ import annotations

import argparse
import asyncio
import random
from collections import defaultdict
from dataclasses import dataclass, field

from python_agent.client import Client
from python_agent.enums import (
    unit_type_name, ORDERS_BY_NAME, UNIT_TYPES_BY_NAME, IDLE_ORDERS,
)
from python_agent.helpers import (
    guess_race, workers, mineral_fields, vespene_geysers, buildings,
    nearest, combat_units, own_refineries,
)


# --------------------------------------------------------------------
# Race catalogs -- everything we want to try to make.
# --------------------------------------------------------------------

@dataclass
class BuildingSpec:
    type_id: int
    cost_min: int
    cost_gas: int = 0
    anchor: str = "any"          # "nexus" | "pylon" | "geyser" | "any"

@dataclass
class UnitSpec:
    type_id: int
    producer_type_id: int
    cost_min: int
    cost_gas: int = 0
    supply_each: int = 0         # in BW-nominal (integer wire value)


# Note: many of these have prereqs (Cybernetics Core needs Gateway;
# Templar Archives needs Citadel; ...) but we don't encode them. We
# fire the build attempt and log the reject if the sim drops it.
PROTOSS_BUILDINGS: list[BuildingSpec] = [
    BuildingSpec(UNIT_TYPES_BY_NAME["Protoss_Gateway"],            150, 0,   "nexus"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Protoss_Forge"],              150, 0,   "nexus"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Protoss_Cybernetics_Core"],   200, 0,   "pylon"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Protoss_Photon_Cannon"],      150, 0,   "pylon"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Protoss_Shield_Battery"],     100, 0,   "pylon"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Protoss_Citadel_of_Adun"],    150, 100, "pylon"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Protoss_Robotics_Facility"],  200, 200, "pylon"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Protoss_Stargate"],           150, 150, "pylon"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Protoss_Templar_Archives"],   150, 200, "pylon"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Protoss_Observatory"],         50, 100, "pylon"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Protoss_Robotics_Support_Bay"],150, 100, "pylon"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Protoss_Fleet_Beacon"],       300, 200, "pylon"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Protoss_Arbiter_Tribunal"],   200, 150, "pylon"),
]

PROTOSS_UNITS: list[UnitSpec] = [
    UnitSpec(UNIT_TYPES_BY_NAME["Protoss_Zealot"],
             UNIT_TYPES_BY_NAME["Protoss_Gateway"], 100, 0, 2),
    UnitSpec(UNIT_TYPES_BY_NAME["Protoss_Dragoon"],
             UNIT_TYPES_BY_NAME["Protoss_Gateway"], 125, 50, 2),
    UnitSpec(UNIT_TYPES_BY_NAME["Protoss_High_Templar"],
             UNIT_TYPES_BY_NAME["Protoss_Gateway"], 50, 150, 2),
    UnitSpec(UNIT_TYPES_BY_NAME["Protoss_Dark_Templar"],
             UNIT_TYPES_BY_NAME["Protoss_Gateway"], 125, 100, 2),
    UnitSpec(UNIT_TYPES_BY_NAME["Protoss_Reaver"],
             UNIT_TYPES_BY_NAME["Protoss_Robotics_Facility"], 200, 100, 4),
    UnitSpec(UNIT_TYPES_BY_NAME["Protoss_Observer"],
             UNIT_TYPES_BY_NAME["Protoss_Robotics_Facility"], 25, 75, 1),
    UnitSpec(UNIT_TYPES_BY_NAME["Protoss_Shuttle"],
             UNIT_TYPES_BY_NAME["Protoss_Robotics_Facility"], 200, 0, 2),
    UnitSpec(UNIT_TYPES_BY_NAME["Protoss_Scout"],
             UNIT_TYPES_BY_NAME["Protoss_Stargate"], 275, 125, 3),
    UnitSpec(UNIT_TYPES_BY_NAME["Protoss_Corsair"],
             UNIT_TYPES_BY_NAME["Protoss_Stargate"], 150, 100, 2),
    UnitSpec(UNIT_TYPES_BY_NAME["Protoss_Carrier"],
             UNIT_TYPES_BY_NAME["Protoss_Stargate"], 350, 250, 6),
    UnitSpec(UNIT_TYPES_BY_NAME["Protoss_Arbiter"],
             UNIT_TYPES_BY_NAME["Protoss_Stargate"], 100, 350, 4),
]

TERRAN_BUILDINGS: list[BuildingSpec] = [
    BuildingSpec(UNIT_TYPES_BY_NAME["Terran_Barracks"],           150, 0,   "cc"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Terran_Engineering_Bay"],    125, 0,   "cc"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Terran_Missile_Turret"],      75, 0,   "cc"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Terran_Bunker"],             100, 0,   "cc"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Terran_Academy"],            150, 0,   "cc"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Terran_Factory"],            200, 100, "cc"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Terran_Starport"],           150, 100, "cc"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Terran_Armory"],             100, 50,  "cc"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Terran_Science_Facility"],   100, 150, "cc"),
]

TERRAN_UNITS: list[UnitSpec] = [
    UnitSpec(UNIT_TYPES_BY_NAME["Terran_Marine"],
             UNIT_TYPES_BY_NAME["Terran_Barracks"], 50, 0, 1),
    UnitSpec(UNIT_TYPES_BY_NAME["Terran_Firebat"],
             UNIT_TYPES_BY_NAME["Terran_Barracks"], 50, 25, 1),
    UnitSpec(UNIT_TYPES_BY_NAME["Terran_Medic"],
             UNIT_TYPES_BY_NAME["Terran_Barracks"], 50, 25, 1),
    UnitSpec(UNIT_TYPES_BY_NAME["Terran_Ghost"],
             UNIT_TYPES_BY_NAME["Terran_Barracks"], 25, 75, 1),
    UnitSpec(UNIT_TYPES_BY_NAME["Terran_Vulture"],
             UNIT_TYPES_BY_NAME["Terran_Factory"], 75, 0, 2),
    UnitSpec(UNIT_TYPES_BY_NAME["Terran_Siege_Tank_Tank_Mode"],
             UNIT_TYPES_BY_NAME["Terran_Factory"], 150, 100, 2),
    UnitSpec(UNIT_TYPES_BY_NAME["Terran_Goliath"],
             UNIT_TYPES_BY_NAME["Terran_Factory"], 100, 50, 2),
    UnitSpec(UNIT_TYPES_BY_NAME["Terran_Wraith"],
             UNIT_TYPES_BY_NAME["Terran_Starport"], 150, 100, 2),
    UnitSpec(UNIT_TYPES_BY_NAME["Terran_Dropship"],
             UNIT_TYPES_BY_NAME["Terran_Starport"], 100, 100, 2),
    UnitSpec(UNIT_TYPES_BY_NAME["Terran_Science_Vessel"],
             UNIT_TYPES_BY_NAME["Terran_Starport"], 100, 225, 2),
    UnitSpec(UNIT_TYPES_BY_NAME["Terran_Battlecruiser"],
             UNIT_TYPES_BY_NAME["Terran_Starport"], 400, 300, 6),
    UnitSpec(UNIT_TYPES_BY_NAME["Terran_Valkyrie"],
             UNIT_TYPES_BY_NAME["Terran_Starport"], 250, 125, 3),
]


def race_catalogs(race: str) -> tuple[list[BuildingSpec], list[UnitSpec],
                                      int, int, int]:
    """Return (buildings, units, worker_type, supply_type, main_type)."""
    if race == "protoss":
        return (PROTOSS_BUILDINGS, PROTOSS_UNITS,
                UNIT_TYPES_BY_NAME["Protoss_Probe"],
                UNIT_TYPES_BY_NAME["Protoss_Pylon"],
                UNIT_TYPES_BY_NAME["Protoss_Nexus"])
    if race == "terran":
        return (TERRAN_BUILDINGS, TERRAN_UNITS,
                UNIT_TYPES_BY_NAME["Terran_SCV"],
                UNIT_TYPES_BY_NAME["Terran_Supply_Depot"],
                UNIT_TYPES_BY_NAME["Terran_Command_Center"])
    # Zerg is out of scope for v2 (drone-morph placement isn't wired).
    raise SystemExit(f"[ai_v2] race={race} not supported by this agent yet")


# --------------------------------------------------------------------
# Pending command tracking -- ground-truth verification of "did this
# actually take effect?"
# --------------------------------------------------------------------

@dataclass
class Pending:
    verb: str                        # "build" | "train"
    target_type: int
    issued_frame: int
    pre_min: int                     # minerals BEFORE fire (from our observation)
    pre_gas: int
    pre_count: int                   # completed + in_progress of target_type
    cost_min: int
    cost_gas: int
    worker_id: int | None = None     # build only

    def label(self) -> str:
        return f"{self.verb}:{unit_type_name(self.target_type)}"


@dataclass
class Stats:
    tried:  dict[str, int] = field(default_factory=lambda: defaultdict(int))
    ack:    dict[str, int] = field(default_factory=lambda: defaultdict(int))
    took:   dict[str, int] = field(default_factory=lambda: defaultdict(int))
    reject: dict[str, int] = field(default_factory=lambda: defaultdict(int))


# --------------------------------------------------------------------
# Helpers.
# --------------------------------------------------------------------

def count_units(units: list[dict], type_id: int) -> tuple[int, int]:
    """(completed, in_progress)."""
    m = [u for u in units if u["type"] == type_id]
    return (sum(1 for u in m if u.get("completed") is True),
            sum(1 for u in m if not u.get("completed", False)))


def find_unit(units: list[dict], unit_id: int) -> dict | None:
    for u in units:
        if u["unit_id"] == unit_id:
            return u
    return None


def own_of_type(units: list[dict], type_id: int, only_complete: bool = True) -> list[dict]:
    return [u for u in units
            if u["type"] == type_id
            and (not only_complete or u.get("completed") is True)]


# --------------------------------------------------------------------
# Verification.
# --------------------------------------------------------------------

def verify_pending(pending: dict, obs: dict, stats: Stats,
                   reject_counts: dict[str, int]) -> None:
    """Check outstanding commands against the observation and log rejects.

    Called every tick BEFORE we fire new commands. Any pending we can
    confirm (or definitively reject) is popped from the dict.
    """
    r = obs["resources"]
    frame = obs["current_frame"]
    units = obs["units"]

    to_drop: list[str] = []

    for key, p in pending.items():
        completed, in_progress = count_units(units, p.target_type)
        cur_count = completed + in_progress
        age = frame - p.issued_frame

        if cur_count > p.pre_count:
            # New unit or in-progress -- command took effect.
            stats.took[p.label()] += 1
            print(f"[ai_v2] TOOK  {p.label():48s} "
                  f"(count {p.pre_count}->{cur_count})")
            to_drop.append(key)
            continue

        # For train, count often doesn't rise until unit pops from
        # queue (~15-30s later). Use resource debit as a proxy: if
        # minerals dropped by ~cost since fire, the sim accepted.
        if p.verb == "train" and p.cost_min > 0:
            spent = p.pre_min - r["minerals"]
            spent_g = p.pre_gas - r["gas"]
            # Allow a fuzz: other trainers may also debit, or we may
            # have gained minerals from ongoing mining. If we spent
            # at least the cost (or an obvious multiple of it),
            # assume accepted.
            if spent >= p.cost_min and spent_g >= p.cost_gas:
                stats.took[p.label()] += 1
                print(f"[ai_v2] TOOK  {p.label():48s} "
                      f"(min-{spent}, gas-{spent_g})")
                to_drop.append(key)
                continue

        # Give the sim a few ticks of grace before declaring reject
        # (walk-to-tile latency, etc.). ~5s of BW frames.
        GRACE = 120
        if age >= GRACE:
            stats.reject[p.label()] += 1
            n = stats.reject[p.label()]
            # Log first occurrence + every 5th.
            if n == 1 or n % 5 == 0:
                print(f"[ai_v2] REJECT {p.label():48s} "
                      f"after {age}f. n={n}. "
                      f"pre: min={p.pre_min} gas={p.pre_gas} "
                      f"count={p.pre_count}. "
                      f"now: min={r['minerals']} gas={r['gas']} "
                      f"supply={r['supply_used']}/{r['supply_max']} "
                      f"count={cur_count}. "
                      f"likely: {_likely_reason(p, r, units)}")
            to_drop.append(key)

    for k in to_drop:
        pending.pop(k, None)


def _likely_reason(p: Pending, r: dict, units: list[dict]) -> str:
    """Heuristic explanation for a silent reject. Not authoritative --
    the sim doesn't tell us, so this is our best guess from state."""
    reasons: list[str] = []
    if r["minerals"] < p.cost_min:
        reasons.append(f"min<{p.cost_min}")
    if r["gas"] < p.cost_gas:
        reasons.append(f"gas<{p.cost_gas}")
    if p.verb == "train":
        # Zerglings come in pairs, supply doubled. Otherwise 2 half-units
        # per nominal supply unit.
        # For coarse estimation, assume supply_each was already accounted
        # for by the specs. We can't recover it from Pending, so skip.
        # (Producer alive?)
        pass
    # Prereq? Photon_Cannon needs Pylon vision; Cyber Core needs Gateway;
    # etc. We list the most common cases.
    tname = unit_type_name(p.target_type)
    prereqs = {
        "Protoss_Cybernetics_Core":   ["Protoss_Gateway"],
        "Protoss_Citadel_of_Adun":    ["Protoss_Cybernetics_Core"],
        "Protoss_Templar_Archives":   ["Protoss_Citadel_of_Adun"],
        "Protoss_Robotics_Support_Bay":["Protoss_Robotics_Facility"],
        "Protoss_Observatory":        ["Protoss_Robotics_Facility"],
        "Protoss_Fleet_Beacon":       ["Protoss_Stargate"],
        "Protoss_Arbiter_Tribunal":   ["Protoss_Stargate",
                                       "Protoss_Templar_Archives"],
        "Protoss_Stargate":           ["Protoss_Cybernetics_Core"],
        "Protoss_Robotics_Facility":  ["Protoss_Cybernetics_Core"],
        "Protoss_Photon_Cannon":      ["Protoss_Forge"],
        "Protoss_Shield_Battery":     ["Protoss_Gateway"],
        "Protoss_Dragoon":            ["Protoss_Cybernetics_Core"],
        "Protoss_High_Templar":       ["Protoss_Templar_Archives"],
        "Protoss_Dark_Templar":       ["Protoss_Templar_Archives"],
        "Protoss_Reaver":             ["Protoss_Robotics_Support_Bay"],
        "Protoss_Observer":           ["Protoss_Observatory"],
        "Protoss_Corsair":            ["Protoss_Stargate"],
        "Protoss_Carrier":            ["Protoss_Fleet_Beacon"],
        "Protoss_Arbiter":            ["Protoss_Arbiter_Tribunal"],
        "Terran_Firebat":             ["Terran_Academy"],
        "Terran_Medic":               ["Terran_Academy"],
        "Terran_Ghost":               ["Terran_Academy",
                                       "Terran_Science_Facility"],
        "Terran_Siege_Tank_Tank_Mode":["Terran_Machine_Shop"],
        "Terran_Goliath":             ["Terran_Armory"],
        "Terran_Wraith":              ["Terran_Starport"],
        "Terran_Dropship":            ["Terran_Control_Tower"],
        "Terran_Science_Vessel":     ["Terran_Science_Facility",
                                       "Terran_Control_Tower"],
        "Terran_Battlecruiser":       ["Terran_Physics_Lab",
                                       "Terran_Control_Tower"],
        "Terran_Valkyrie":            ["Terran_Armory",
                                       "Terran_Control_Tower"],
    }
    for prereq_name in prereqs.get(tname, []):
        prereq_id = UNIT_TYPES_BY_NAME[prereq_name]
        c, _ = count_units(units, prereq_id)
        if c == 0:
            reasons.append(f"missing:{prereq_name}")
    if not reasons:
        reasons.append("unknown (placement? supply? sim state?)")
    return ", ".join(reasons)


# --------------------------------------------------------------------
# Actions. Each returns a Pending (if fired) or None.
# --------------------------------------------------------------------

async def try_train_worker(c: Client, obs: dict, worker_type: int,
                           main_type: int, cost_min: int) -> Pending | None:
    r = obs["resources"]
    if r["minerals"] < cost_min:
        return None
    # Any idle main? (Nexus/CC. Note: Nexus training a probe stays in
    # primary order Nothing; the train queue is a secondary order,
    # invisible on wire. So just fire on any completed main; sim
    # rejects if queue full.)
    mains = own_of_type(obs["units"], main_type, only_complete=True)
    if not mains:
        return None
    p = mains[0]
    completed, in_progress = count_units(obs["units"], worker_type)
    try:
        await c.train(unit_id=p["unit_id"], unit_type=worker_type)
    except Exception as e:
        print(f"[ai_v2]  cmd error train worker: {e}")
        return None
    return Pending(
        verb="train", target_type=worker_type,
        issued_frame=obs["current_frame"],
        pre_min=r["minerals"], pre_gas=r["gas"],
        pre_count=completed + in_progress,
        cost_min=cost_min, cost_gas=0,
    )


async def try_build(c: Client, obs: dict, spec: BuildingSpec,
                    worker_type: int, main_type: int, supply_type: int,
                    pending_workers: set[int]) -> Pending | None:
    r = obs["resources"]
    if r["minerals"] < spec.cost_min or r["gas"] < spec.cost_gas:
        return None

    units = obs["units"]

    # Pick worker (any free-ish probe not tied to another pending build).
    cands = [u for u in workers(units)
             if u["type"] == worker_type
             and u["unit_id"] not in pending_workers]
    if not cands:
        return None
    worker = cands[0]

    # Pick placement anchor.
    kwargs: dict = {
        "unit_type": spec.type_id,
        "worker_unit": worker["unit_id"],
        "radius_tiles": 20,
        "max_results": 8,
    }
    if spec.anchor == "geyser":
        gs = vespene_geysers(obs.get("neutrals", []))
        if not gs:
            return None
        g = gs[0]
        kwargs["center_x"] = g["x"]
        kwargs["center_y"] = g["y"]
        kwargs["radius_tiles"] = 3
    elif spec.anchor == "pylon":
        pylons = own_of_type(units, supply_type, only_complete=True)
        if pylons:
            anchor = random.choice(pylons)
            kwargs["center_x"] = anchor["x"]
            kwargs["center_y"] = anchor["y"]
        # else: let placement fall back to worker-anchored search
    elif spec.anchor in ("nexus", "cc"):
        mains = own_of_type(units, main_type, only_complete=True)
        if mains:
            anchor = mains[0]
            kwargs["center_x"] = anchor["x"]
            kwargs["center_y"] = anchor["y"]

    try:
        resp = await c.find_placement(**kwargs)
    except Exception as e:
        print(f"[ai_v2]  find_placement error for "
              f"{unit_type_name(spec.type_id)}: {e}")
        return None
    spots = resp.get("spots", [])
    if not spots:
        # Not a "reject" -- just no valid tile visible. Common for
        # pylon-anchored buildings when Pylon isn't up yet.
        return None
    spot = spots[0]

    completed, in_progress = count_units(units, spec.type_id)
    try:
        await c.build(unit_id=worker["unit_id"], unit_type=spec.type_id,
                      tile_x=spot["tile_x"], tile_y=spot["tile_y"])
    except Exception as e:
        print(f"[ai_v2]  build cmd error {unit_type_name(spec.type_id)}: {e}")
        return None
    return Pending(
        verb="build", target_type=spec.type_id,
        issued_frame=obs["current_frame"],
        pre_min=r["minerals"], pre_gas=r["gas"],
        pre_count=completed + in_progress,
        cost_min=spec.cost_min, cost_gas=spec.cost_gas,
        worker_id=worker["unit_id"],
    )


async def try_train_unit(c: Client, obs: dict, spec: UnitSpec) -> Pending | None:
    r = obs["resources"]
    if r["minerals"] < spec.cost_min or r["gas"] < spec.cost_gas:
        return None
    prods = own_of_type(obs["units"], spec.producer_type_id, only_complete=True)
    if not prods:
        return None
    # Ideally pick one that isn't already training. Wire order is
    # always "Nothing" during Train secondary order, so we can't tell.
    # Just try the first one; sim rejects if queue full.
    p = prods[0]
    completed, in_progress = count_units(obs["units"], spec.type_id)
    try:
        await c.train(unit_id=p["unit_id"], unit_type=spec.type_id)
    except Exception as e:
        print(f"[ai_v2]  train cmd error {unit_type_name(spec.type_id)}: {e}")
        return None
    return Pending(
        verb="train", target_type=spec.type_id,
        issued_frame=obs["current_frame"],
        pre_min=r["minerals"], pre_gas=r["gas"],
        pre_count=completed + in_progress,
        cost_min=spec.cost_min, cost_gas=spec.cost_gas,
    )


# Orders that mean "already busy on a resource run". Don't disturb them
# with another gather order.
_GAS_ORDERS   = {ORDERS_BY_NAME["MoveToGas"], ORDERS_BY_NAME["WaitForGas"],
                 ORDERS_BY_NAME["HarvestGas"], ORDERS_BY_NAME["ReturnGas"]}
_MIN_ORDERS   = {ORDERS_BY_NAME["Harvest1"], ORDERS_BY_NAME["Harvest2"],
                 ORDERS_BY_NAME["MoveToMinerals"],
                 ORDERS_BY_NAME["WaitForMinerals"],
                 ORDERS_BY_NAME["MiningMinerals"],
                 ORDERS_BY_NAME["ReturnMinerals"]}


async def phase_mine(c: Client, obs: dict, worker_type: int,
                     pending_workers: set[int],
                     target_gas_workers: int) -> None:
    """Send idle workers to minerals; top up gas workers per refinery.

    Without this, workers spawn in PlayerGuard and never mine, minerals
    stay pinned at 50, and every build/train silently rejects on
    insufficient resources.
    """
    units = obs["units"]
    wu = [u for u in workers(units) if u["type"] == worker_type
          and u["unit_id"] not in pending_workers]
    mfs = mineral_fields(obs.get("neutrals", []))
    refineries = own_refineries(units)

    # ---- 1. Gas assignments first (higher value per trip). ----
    if refineries:
        on_gas_now = sum(1 for u in wu if u["order"] in _GAS_ORDERS)
        need = len(refineries) * target_gas_workers - on_gas_now
        if need > 0:
            # Prefer idle; then poach mineral trips.
            pool = [u for u in wu if u["order"] in IDLE_ORDERS]
            if len(pool) < need:
                pool.extend(u for u in wu
                            if u["order"] in _MIN_ORDERS
                            and u not in pool)
            for w in pool[:need]:
                target = nearest(w, refineries)
                if target is None:
                    break
                try:
                    await c.gather(unit_id=w["unit_id"],
                                   target_unit=target["unit_id"])
                    print(f"[ai_v2]  MINE worker {w['unit_id']} -> "
                          f"gas at refinery {target['unit_id']}")
                except Exception as e:
                    print(f"[ai_v2]  gather-gas error: {e}")

    # ---- 2. Idle -> minerals. ----
    if mfs:
        for w in wu:
            if w["order"] not in IDLE_ORDERS:
                continue
            m = nearest(w, mfs)
            if m is None:
                break
            try:
                await c.gather(unit_id=w["unit_id"], target_unit=m["unit_id"])
                print(f"[ai_v2]  MINE worker {w['unit_id']} -> "
                      f"mineral {m['unit_id']}")
            except Exception as e:
                print(f"[ai_v2]  gather-min error: {e}")


# --------------------------------------------------------------------
# Main loop.
# --------------------------------------------------------------------

async def run(c: Client, interval_sec: float,
              worker_target: int, supply_slack: int,
              worker_train_min: int) -> None:
    print(f"[ai_v2] connected as slot={c.welcome.slot} "
          f"at frame={c.welcome.current_frame}")

    map_info = (await c.observe(targets=["map_info"]))["map_info"]
    map_w, map_h = map_info["width"], map_info["height"]

    race: str | None = None
    catalog_buildings: list[BuildingSpec] = []
    catalog_units: list[UnitSpec] = []
    worker_type = supply_type = main_type = None

    # Pending: keyed by (verb, target_type). One outstanding per target.
    pending: dict[str, Pending] = {}
    stats = Stats()
    reject_counts: dict[str, int] = defaultdict(int)

    # Attack target: assume opposite corner.
    home_x = home_y = None
    tgt_x = tgt_y = None

    # One-shot: exercise move + stop verbs early for coverage.
    move_done = False
    stop_done = False

    while True:
        obs = await c.observe(
            targets=["units", "resources", "enemies", "neutrals"])
        frame = obs["current_frame"]
        r = obs["resources"]
        units = obs["units"]

        if race is None:
            race = guess_race(units)
            (catalog_buildings, catalog_units,
             worker_type, supply_type, main_type) = race_catalogs(race)
            if units:
                home_x = sum(u["x"] for u in units) // len(units)
                home_y = sum(u["y"] for u in units) // len(units)
                tgt_x = map_w - home_x
                tgt_y = map_h - home_y
            print(f"[ai_v2] race={race} worker_type={worker_type} "
                  f"home=({home_x},{home_y}) target=({tgt_x},{tgt_y})")

        # ---- Verify pending commands from previous ticks. ----
        verify_pending(pending, obs, stats, reject_counts)

        # ---- Status line ----
        n_workers = len(workers(units))
        n_combat = len(combat_units(units))
        n_bldgs = len(buildings(units))
        # How many distinct building/unit types have we produced?
        b_types_owned = sum(1 for s in catalog_buildings
                            if count_units(units, s.type_id)[0] > 0
                            or count_units(units, s.type_id)[1] > 0)
        u_types_seen  = sum(1 for s in catalog_units
                            if count_units(units, s.type_id)[0] > 0
                            or count_units(units, s.type_id)[1] > 0)
        print(f"[ai_v2] frame={frame} race={race} "
              f"min={r['minerals']} gas={r['gas']} "
              f"supply={r['supply_used']}/{r['supply_max']} "
              f"workers={n_workers}/{worker_target} combat={n_combat} "
              f"bldgs={n_bldgs} types_built={b_types_owned}/{len(catalog_buildings)} "
              f"types_trained={u_types_seen}/{len(catalog_units)} "
              f"pending={len(pending)}")

        # Track workers already committed to a pending build so we don't
        # yank them onto another build in the same tick.
        pending_workers = {p.worker_id for p in pending.values()
                           if p.worker_id is not None}

        # ---- 0. Mine. Without this, minerals stay at 50 forever and
        #        every build/train rejects on cost. Runs FIRST so a
        #        newborn worker can head to minerals in the same tick
        #        it appears.
        await phase_mine(c, obs, worker_type, pending_workers,
                         target_gas_workers=3)

        # ---- Decision list (top-down, first missing = try this tick). ----
        # We fire at most ONE new command per (verb, target) so verify
        # remains unambiguous. Multiple different (verb, target) can
        # fire in the same tick (e.g. train probe + build gateway).

        # 1. Worker cap.
        if (n_workers < worker_target
                and "train:%d" % worker_type not in pending):
            p = await try_train_worker(c, obs, worker_type,
                                       main_type, worker_train_min)
            if p is not None:
                pending[f"train:{worker_type}"] = p
                stats.tried["train:" + unit_type_name(worker_type)] += 1
                print(f"[ai_v2] FIRE  train:{unit_type_name(worker_type)}")
                # Re-read obs cheaply -- we already updated pending.
                # For remaining actions we still use the same obs; that's fine.

        # 2. Supply. Build another Pylon when gap < supply_slack.
        gap = r["supply_max"] - r["supply_used"]
        _, sup_in_progress = count_units(units, supply_type)
        if gap < supply_slack and sup_in_progress == 0:
            if f"build:{supply_type}" not in pending:
                # Anchor a supply build at a RANDOM own building so pylons
                # spread across the base.
                own_bldgs = [u for u in units if u.get("building")]
                anchor: dict | None = random.choice(own_bldgs) if own_bldgs else None
                spec = BuildingSpec(supply_type,
                                    cost_min=100 if race == "protoss" else 100,
                                    anchor="nexus")
                # Custom: override anchor coords via a mini-spec.
                cands = [u for u in workers(units)
                         if u["type"] == worker_type
                         and u["unit_id"] not in pending_workers]
                if cands and anchor is not None:
                    worker = cands[0]
                    kwargs = {"unit_type": supply_type,
                              "worker_unit": worker["unit_id"],
                              "center_x": anchor["x"],
                              "center_y": anchor["y"],
                              "radius_tiles": 15,
                              "max_results": 8}
                    try:
                        resp = await c.find_placement(**kwargs)
                        spots = resp.get("spots", [])
                        if spots:
                            spot = spots[0]
                            completed, ipg = count_units(units, supply_type)
                            await c.build(unit_id=worker["unit_id"],
                                          unit_type=supply_type,
                                          tile_x=spot["tile_x"],
                                          tile_y=spot["tile_y"])
                            pending[f"build:{supply_type}"] = Pending(
                                verb="build", target_type=supply_type,
                                issued_frame=frame,
                                pre_min=r["minerals"], pre_gas=r["gas"],
                                pre_count=completed + ipg,
                                cost_min=100, cost_gas=0,
                                worker_id=worker["unit_id"],
                            )
                            stats.tried["build:" + unit_type_name(supply_type)] += 1
                            print(f"[ai_v2] FIRE  build:{unit_type_name(supply_type)} "
                                  f"@tile ({spot['tile_x']},{spot['tile_y']})")
                    except Exception as e:
                        print(f"[ai_v2]  pylon fire error: {e}")

        # Refresh the worker-busy set after supply build.
        pending_workers = {p.worker_id for p in pending.values()
                           if p.worker_id is not None}

        # 3. Every building we don't have yet: try to build one.
        for spec in catalog_buildings:
            key = f"build:{spec.type_id}"
            if key in pending:
                continue
            completed, in_progress = count_units(units, spec.type_id)
            if completed + in_progress > 0:
                continue
            p = await try_build(c, obs, spec, worker_type, main_type,
                                supply_type, pending_workers)
            if p is not None:
                pending[key] = p
                pending_workers.add(p.worker_id)  # type: ignore
                stats.tried["build:" + unit_type_name(spec.type_id)] += 1
                print(f"[ai_v2] FIRE  build:{unit_type_name(spec.type_id)}")

        # 3b. Assimilator is the first "gas" building. Include it as a
        # geyser-anchored build too. It's already in catalog_buildings
        # for Terran; for Protoss the equivalent Assimilator wasn't in
        # PROTOSS_BUILDINGS -- add explicitly.
        if race == "protoss":
            assim_type = UNIT_TYPES_BY_NAME["Protoss_Assimilator"]
            key = f"build:{assim_type}"
            if key not in pending:
                completed, in_progress = count_units(units, assim_type)
                if completed + in_progress == 0:
                    p = await try_build(
                        c, obs,
                        BuildingSpec(assim_type, 100, 0, "geyser"),
                        worker_type, main_type, supply_type,
                        pending_workers)
                    if p is not None:
                        pending[key] = p
                        pending_workers.add(p.worker_id)  # type: ignore
                        stats.tried[f"build:{unit_type_name(assim_type)}"] += 1
                        print(f"[ai_v2] FIRE  build:{unit_type_name(assim_type)}")
        elif race == "terran":
            ref_type = UNIT_TYPES_BY_NAME["Terran_Refinery"]
            key = f"build:{ref_type}"
            if key not in pending:
                completed, in_progress = count_units(units, ref_type)
                if completed + in_progress == 0:
                    p = await try_build(
                        c, obs,
                        BuildingSpec(ref_type, 100, 0, "geyser"),
                        worker_type, main_type, supply_type,
                        pending_workers)
                    if p is not None:
                        pending[key] = p
                        pending_workers.add(p.worker_id)  # type: ignore
                        stats.tried[f"build:{unit_type_name(ref_type)}"] += 1
                        print(f"[ai_v2] FIRE  build:{unit_type_name(ref_type)}")

        # 4. Every unit we haven't trained yet.
        for spec in catalog_units:
            key = f"train:{spec.type_id}"
            if key in pending:
                continue
            completed, in_progress = count_units(units, spec.type_id)
            if completed + in_progress > 0:
                continue
            p = await try_train_unit(c, obs, spec)
            if p is not None:
                pending[key] = p
                stats.tried["train:" + unit_type_name(spec.type_id)] += 1
                print(f"[ai_v2] FIRE  train:{unit_type_name(spec.type_id)}")

        # 5. Coverage: exercise move + stop once each.
        if not move_done:
            idle = [u for u in workers(units) if u["order"] in IDLE_ORDERS
                    and u["unit_id"] not in pending_workers]
            if idle and home_x is not None:
                w = idle[0]
                dst_x = home_x + random.randint(-200, 200)
                dst_y = home_y + random.randint(-200, 200)
                try:
                    await c.move(unit_id=w["unit_id"], x=dst_x, y=dst_y)
                    print(f"[ai_v2] FIRE  move worker {w['unit_id']} "
                          f"-> ({dst_x},{dst_y})  [coverage]")
                    move_done = True
                except Exception as e:
                    print(f"[ai_v2]  move error: {e}")
        if move_done and not stop_done:
            # Pick any idle unit, send stop.
            cands = [u for u in units if u["order"] not in IDLE_ORDERS
                     and not u.get("building")]
            if cands:
                w = cands[0]
                try:
                    await c.stop(unit_id=w["unit_id"])
                    print(f"[ai_v2] FIRE  stop {unit_type_name(w['type'])} "
                          f"{w['unit_id']}  [coverage]")
                    stop_done = True
                except Exception as e:
                    print(f"[ai_v2]  stop error: {e}")

        # 6. Attack: any combat unit not already attacking → attack-move.
        for u in combat_units(units):
            if u["order"] not in IDLE_ORDERS:
                continue
            try:
                enemies = obs.get("enemies", [])
                if enemies:
                    t = nearest(u, enemies)
                    if t is not None:
                        await c.attack(unit_id=u["unit_id"],
                                       target_unit=t["unit_id"])
                        continue
                if tgt_x is not None:
                    await c.attack(unit_id=u["unit_id"], target_unit=0,
                                   x=tgt_x, y=tgt_y)
            except Exception as e:
                print(f"[ai_v2]  attack error: {e}")

        await asyncio.sleep(interval_sec)


async def main(api_key: str, host: str, port: int, interval_sec: float,
               worker_target: int, supply_slack: int,
               worker_train_min: int) -> None:
    async with Client(api_key=api_key, host=host, port=port) as c:
        await run(c, interval_sec, worker_target, supply_slack,
                  worker_train_min)


def entrypoint() -> None:
    p = argparse.ArgumentParser(
        prog="python3 -m python_agent.agents.ai_v2_agent",
        description="Coverage-oriented agent that tries to build/train "
                    "one of every race-appropriate thing.")
    p.add_argument("api_key")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=6113)
    p.add_argument("--interval-sec", type=float, default=1.5)
    p.add_argument("--worker-target", type=int, default=40,
                   help="cap on worker count (default 40)")
    p.add_argument("--supply-slack", type=int, default=8,
                   help="build another supply structure when "
                        "(supply_max - supply_used) drops below this")
    p.add_argument("--worker-train-min", type=int, default=50)
    args = p.parse_args()
    try:
        asyncio.run(main(args.api_key, args.host, args.port,
                         args.interval_sec, args.worker_target,
                         args.supply_slack, args.worker_train_min))
    except KeyboardInterrupt:
        print("\n[ai_v2] stopped")


if __name__ == "__main__":
    entrypoint()
