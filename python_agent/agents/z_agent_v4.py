"""z_agent_v4: Zerg counterpart of p_agent_v4 / t_agent_v4.

Zerg's production model is architecturally different from Protoss and
Terran and requires two new server verbs (`morph`, `morph_building`)
that landed alongside this agent. Everything below assumes those verbs
are present.

Same v3-derived infrastructure (scouting, expansion, priority-ordered
decision loop, verify_pending) with these Zerg-specific changes:

  * All units come from Larva morphs, not producer buildings. The
    Larva pool sits at each Hatchery/Lair/Hive. `try_morph_unit`
    picks an idle Larva and fires `c.morph(larva_id, target_type)`.

  * All buildings come from Drone morphs, not a builder building.
    `try_build_zerg` picks a Drone and fires `c.build(drone_id,
    building_type, tile_x, tile_y, order=25)`. Order 25 is
    Orders::DroneStartBuild; the Drone is consumed and becomes the
    building.

  * Supply structures are Overlords (units), not buildings. When
    supply is short, the "pylon pass" morphs a Larva to Overlord.

  * Building tier morphs (Hatch->Lair->Hive) use the new
    `morph_building` verb -- fired on the existing building itself,
    which stays in place while its type_id changes.

  * v4 flagship: **Larva keepup**. No Larva ever idles. Priority 7.5
    scans for idle own Larvae and morphs each one to the currently-
    most-needed unit type (Drone under worker_target, Overlord if
    supply tight, Zergling for army, etc.). Mirrors the "auto-refill"
    spirit of Protoss v4's fighter maintenance and Terran v4's SCV
    repair.

Egg intermediate state: Zerg unit morphs go through Zerg_Egg (36)
for ~5 seconds before hatching. Buildings go through the building
itself with `completed=False`. Since eggs share a single type_id and
we can't see their build_queue on the wire, we conservatively count
"any own Egg" as gating for further Larva morphs -- a single-pass-
per-tick throttle prevents spamming morphs on the same Larva.

Usage:
    python3 -m python_agent.agents.z_agent_v4 <api_key>

Prereqs: server must include the `morph`/`morph_building` verbs and
`build` verb must accept an `order` override (all three are in
current server/agent_protocol.h).
"""

from __future__ import annotations

import argparse
import asyncio
import random
from collections import defaultdict
from dataclasses import dataclass, field

from python_agent.client import Client
from python_agent.enums import (
    unit_type_name, order_name,
    ORDERS_BY_NAME, UNIT_TYPES_BY_NAME, IDLE_ORDERS,
)
from python_agent.helpers import (
    guess_race, workers, mineral_fields, vespene_geysers, buildings,
    nearest, combat_units, own_refineries, dist_pixels,
    radial_waypoints, zscan_waypoints,
)


# --------------------------------------------------------------------
# Race catalogs.
# --------------------------------------------------------------------

# Orders::DroneStartBuild -- the order override we must pass to c.build
# when a Drone starts a Zerg building. Matches bwenums.h enumeration
# position (25 counting from the first entry Die=0).
ORDER_DRONE_START_BUILD = 25


@dataclass
class BuildingSpec:
    type_id: int
    cost_min: int
    cost_gas: int = 0
    # For Zerg, most placement anchors around an existing Hatchery (or
    # Lair/Hive -- they morph in place, keeping their location). We use
    # "hatchery" for those, "geyser" for Extractor, "any" for a
    # roaming-style placement that spreads.
    anchor: str = "hatchery"


@dataclass
class UnitSpec:
    type_id: int
    # Producer for Zerg is ALWAYS Zerg_Larva (35). Kept in the shape for
    # symmetry with p_agent_v4 / t_agent_v4, but the code paths that
    # look up producer buildings are replaced by "find any own Larva".
    producer_type_id: int
    cost_min: int
    cost_gas: int = 0
    supply_each: int = 0
    target_count: int = 1
    # A Hatchery is needed to spawn Larvae; a Lair or Hive is needed for
    # some tier-2/3 morphs (Mutalisk needs Spire, etc.). But the actual
    # prereq check happens sim-side: if unit_can_build fails, the sim
    # silent-rejects. This field is documentation only.
    tier: int = 1                # 1=Hatch, 2=Lair, 3=Hive


@dataclass
class UpgradeSpec:
    kind: str                    # "upgrade" | "research"
    enum_id: int
    source_type_id: int
    cost_min: int
    cost_gas: int
    label: str


# Zerg buildings -- most anchor on the Hatchery so they stay on creep.
# Extractor uniquely anchors on geyser. Spawning_Pool must come first
# so tech unlocks Zerglings + Sunken; Extractor comes after so we have
# gas for tier-2. Values from bwenums.h + Liquipedia costs.
ZERG_BUILDINGS: list[BuildingSpec] = [
    BuildingSpec(UNIT_TYPES_BY_NAME["Zerg_Spawning_Pool"],       200,   0, "hatchery"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Zerg_Extractor"],            50,   0, "geyser"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Zerg_Evolution_Chamber"],    75,   0, "hatchery"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Zerg_Hydralisk_Den"],       100,  50, "hatchery"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Zerg_Creep_Colony"],         75,   0, "hatchery"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Zerg_Spire"],               200, 150, "hatchery"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Zerg_Queens_Nest"],         150, 100, "hatchery"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Zerg_Defiler_Mound"],       100, 100, "hatchery"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Zerg_Ultralisk_Cavern"],    150, 200, "hatchery"),
]

# Zerg units -- all producer_type_id is Zerg_Larva. Overlord is
# produced by the flagship "Larva keepup" pass when supply is short,
# so it's NOT listed here (would double-fire against pylon logic).
_LARVA = UNIT_TYPES_BY_NAME["Zerg_Larva"]
ZERG_UNITS: list[UnitSpec] = [
    UnitSpec(UNIT_TYPES_BY_NAME["Zerg_Zergling"], _LARVA,  50,   0, 1, target_count=16, tier=1),
    UnitSpec(UNIT_TYPES_BY_NAME["Zerg_Hydralisk"], _LARVA, 75,  25, 2, target_count=8,  tier=1),
    UnitSpec(UNIT_TYPES_BY_NAME["Zerg_Mutalisk"], _LARVA, 100, 100, 4, target_count=4,  tier=2),
    UnitSpec(UNIT_TYPES_BY_NAME["Zerg_Scourge"],  _LARVA,  25,  75, 1, target_count=4,  tier=2),
    UnitSpec(UNIT_TYPES_BY_NAME["Zerg_Queen"],    _LARVA, 100, 100, 4, target_count=1,  tier=2),
    UnitSpec(UNIT_TYPES_BY_NAME["Zerg_Defiler"],  _LARVA,  50, 150, 4, target_count=1,  tier=3),
    UnitSpec(UNIT_TYPES_BY_NAME["Zerg_Ultralisk"], _LARVA, 200, 200, 8, target_count=2, tier=3),
]

# Zerg upgrades from bwenums.h:
#   UpgradeTypes: Zerg_Carapace=3, Zerg_Flyer_Carapace=4,
#                 Zerg_Melee_Attacks=10, Zerg_Missile_Attacks=11,
#                 Zerg_Flyer_Attacks=12, Ventral_Sacs=24, Antennae=25,
#                 Pneumatized_Carapace=26, Metabolic_Boost=27,
#                 Adrenal_Glands=28, Muscular_Augments=29,
#                 Grooved_Spines=30, Gamete_Meiosis=31,
#                 Chitinous_Plating=52, Anabolic_Synthesis=53
#   TechTypes: Burrowing=11, Lurker_Aspect=32
ZERG_UPGRADES: list[UpgradeSpec] = [
    # Evolution Chamber -- ground weapon / armor.
    UpgradeSpec("upgrade", 10, UNIT_TYPES_BY_NAME["Zerg_Evolution_Chamber"],
                100, 100, "MeleeAttacks_L1"),
    UpgradeSpec("upgrade", 11, UNIT_TYPES_BY_NAME["Zerg_Evolution_Chamber"],
                100, 100, "MissileAttacks_L1"),
    UpgradeSpec("upgrade", 3, UNIT_TYPES_BY_NAME["Zerg_Evolution_Chamber"],
                150, 150, "Carapace_L1"),
    # Spawning Pool -- Zergling upgrades + Burrowing tech.
    UpgradeSpec("upgrade", 27, UNIT_TYPES_BY_NAME["Zerg_Spawning_Pool"],
                100, 100, "MetabolicBoost"),
    UpgradeSpec("upgrade", 28, UNIT_TYPES_BY_NAME["Zerg_Spawning_Pool"],
                200, 200, "AdrenalGlands"),
    UpgradeSpec("research", 11, UNIT_TYPES_BY_NAME["Zerg_Spawning_Pool"],
                100, 100, "Burrowing"),
    # Hydralisk Den -- Hydra upgrades + Lurker tech.
    UpgradeSpec("upgrade", 29, UNIT_TYPES_BY_NAME["Zerg_Hydralisk_Den"],
                150, 150, "MuscularAugments"),
    UpgradeSpec("upgrade", 30, UNIT_TYPES_BY_NAME["Zerg_Hydralisk_Den"],
                150, 150, "GroovedSpines"),
    UpgradeSpec("research", 32, UNIT_TYPES_BY_NAME["Zerg_Hydralisk_Den"],
                200, 200, "LurkerAspect"),
    # Spire -- flyer weapons/armor.
    UpgradeSpec("upgrade", 12, UNIT_TYPES_BY_NAME["Zerg_Spire"],
                100, 100, "FlyerAttacks_L1"),
    UpgradeSpec("upgrade", 4, UNIT_TYPES_BY_NAME["Zerg_Spire"],
                150, 150, "FlyerCarapace_L1"),
]


def race_catalogs(race: str):
    if race == "zerg":
        return (ZERG_BUILDINGS, ZERG_UNITS, ZERG_UPGRADES,
                UNIT_TYPES_BY_NAME["Zerg_Drone"],
                UNIT_TYPES_BY_NAME["Zerg_Overlord"],
                UNIT_TYPES_BY_NAME["Zerg_Hatchery"])
    raise SystemExit(f"[z_v4] race={race} not supported (use p_agent_v4 for "
                     f"protoss or t_agent_v5 for terran)")


# --------------------------------------------------------------------
# Pending tracking.
# --------------------------------------------------------------------

@dataclass
class Pending:
    verb: str
    target_type: int
    issued_frame: int
    pre_min: int
    pre_gas: int
    pre_count: int
    cost_min: int
    cost_gas: int
    worker_id: int | None = None

    def label(self) -> str:
        return f"{self.verb}:{unit_type_name(self.target_type)}"


@dataclass
class Stats:
    tried: dict[str, int] = field(default_factory=lambda: defaultdict(int))
    took: dict[str, int] = field(default_factory=lambda: defaultdict(int))
    reject: dict[str, int] = field(default_factory=lambda: defaultdict(int))


# --------------------------------------------------------------------
# Helpers.
# --------------------------------------------------------------------

_EGG_TYPE = UNIT_TYPES_BY_NAME["Zerg_Egg"]
_LURKER_EGG = UNIT_TYPES_BY_NAME["Zerg_Lurker_Egg"]
_COCOON = UNIT_TYPES_BY_NAME["Zerg_Cocoon"]
_MORPH_EGG_TYPES = {_EGG_TYPE, _LURKER_EGG, _COCOON}


def count_units(units: list[dict], type_id: int) -> tuple[int, int]:
    m = [u for u in units if u["type"] == type_id]
    return (sum(1 for u in m if u.get("completed") is True),
            sum(1 for u in m if not u.get("completed", False)))


def count_eggs(units: list[dict]) -> int:
    """All morph eggs owned by us -- conservative gate to avoid firing
    a second Larva morph while one is already in flight.

    Zerg_Egg / Zerg_Lurker_Egg / Zerg_Cocoon are the three egg types.
    We can't tell what a given egg is morphing into (no build_queue on
    wire), so we treat any egg as "a morph is happening"."""
    return sum(1 for u in units if u["type"] in _MORPH_EGG_TYPES)


def find_unit(units: list[dict], unit_id: int) -> dict | None:
    for u in units:
        if u["unit_id"] == unit_id:
            return u
    return None


def own_of_type(units: list[dict], type_id: int, only_complete=True) -> list[dict]:
    return [u for u in units if u["type"] == type_id
            and (not only_complete or u.get("completed") is True)]


def idle_larvae(units: list[dict]) -> list[dict]:
    """Own Larvae not already carrying a morph order."""
    lv = own_of_type(units, _LARVA)
    return [u for u in lv if u["order"] not in (
        ORDERS_BY_NAME.get("ZergUnitMorph", -1),
        ORDERS_BY_NAME.get("ZergBirth", -1),
    )]


_GAS_ORDERS = {ORDERS_BY_NAME[n] for n in
               ("MoveToGas", "WaitForGas", "HarvestGas", "ReturnGas")}
_MIN_ORDERS = {ORDERS_BY_NAME[n] for n in
               ("Harvest1", "Harvest2", "MoveToMinerals", "WaitForMinerals",
                "MiningMinerals", "ReturnMinerals")}


# --------------------------------------------------------------------
# Verify pending.
# --------------------------------------------------------------------

def verify_pending(pending: dict, obs: dict, stats: Stats,
                   grace_frames: int) -> None:
    r = obs["resources"]
    frame = obs["current_frame"]
    units = obs["units"]
    to_drop = []
    for key, p in pending.items():
        completed, in_progress = count_units(units, p.target_type)
        cur_count = completed + in_progress
        age = frame - p.issued_frame
        if cur_count > p.pre_count:
            stats.took[p.label()] += 1
            print(f"[z_v4] TOOK  {p.label():48s} "
                  f"(count {p.pre_count}->{cur_count} after {age}f)")
            to_drop.append(key)
            continue
        if age >= grace_frames:
            stats.reject[p.label()] += 1
            n = stats.reject[p.label()]
            if n == 1 or n % 5 == 0:
                print(f"[z_v4] REJECT {p.label():48s} after {age}f. n={n}. "
                      f"pre min={p.pre_min} gas={p.pre_gas} count={p.pre_count}; "
                      f"now min={r['minerals']} gas={r['gas']} count={cur_count}")
            to_drop.append(key)
    for k in to_drop:
        pending.pop(k, None)


# --------------------------------------------------------------------
# Mining phase.
# --------------------------------------------------------------------

async def phase_mine(c: Client, obs: dict, worker_type: int,
                     busy_workers: set[int],
                     target_gas_workers: int) -> set[int]:
    units = obs["units"]
    wu = [u for u in workers(units) if u["type"] == worker_type
          and u["unit_id"] not in busy_workers]
    mfs = mineral_fields(obs.get("neutrals", []))
    refineries = own_refineries(units)
    just_assigned: set[int] = set()

    if refineries:
        on_gas_now = sum(1 for u in wu if u["order"] in _GAS_ORDERS)
        need = len(refineries) * target_gas_workers - on_gas_now
        if need > 0:
            pool = [u for u in wu if u["order"] in IDLE_ORDERS]
            if len(pool) < need:
                pool.extend(u for u in wu if u["order"] in _MIN_ORDERS
                            and u not in pool)
            for w in pool[:need]:
                target = nearest(w, refineries)
                if target is None:
                    break
                try:
                    await c.gather(unit_id=w["unit_id"],
                                   target_unit=target["unit_id"])
                    just_assigned.add(w["unit_id"])
                except Exception as e:
                    print(f"[z_v4]  gather-gas error: {e}")

    if mfs:
        for w in wu:
            if w["order"] not in IDLE_ORDERS:
                continue
            m = nearest(w, mfs)
            if m is None:
                break
            try:
                await c.gather(unit_id=w["unit_id"], target_unit=m["unit_id"])
                just_assigned.add(w["unit_id"])
            except Exception as e:
                print(f"[z_v4]  gather-min error: {e}")

    return just_assigned


# --------------------------------------------------------------------
# Scouting (identical to p_agent_v4).
# --------------------------------------------------------------------

@dataclass
class Scout:
    worker_id: int
    mode: str
    waypoint_idx: int
    arrived_frame: int = -1
    pos_history: list[tuple[int, int]] = field(default_factory=list)
    dist_history: list[float] = field(default_factory=list)
    wp_started_frame: int = -1
    wp_start_dist: float = 0.0
    blacklist: set[int] = field(default_factory=set)


@dataclass
class KnownEnemy:
    unit_id: int
    type_id: int
    x: int
    y: int
    first_seen_frame: int


ARRIVE_RADIUS = 200
STUCK_RADIUS_PX = 100
STUCK_WINDOW = 4
PROGRESS_MIN_DELTA_PX = 200
PROGRESS_WINDOW = 8
STUCK_TIMEOUT_FRAMES = 3000


async def phase_scout(c: Client, obs: dict, worker_type: int,
                      scouts: dict[int, Scout],
                      waypoints_by_mode: dict[str, list[tuple[int, int]]],
                      known_enemies: dict[int, KnownEnemy],
                      known_resources: dict[int, tuple[int, int, int]],
                      target_by_mode: dict[str, int],
                      busy_workers: set[int],
                      home_x: int, home_y: int) -> set[int]:
    units = obs["units"]
    frame = obs["current_frame"]

    live_ids = {u["unit_id"] for u in units}
    for wid in list(scouts.keys()):
        if wid not in live_ids:
            print(f"[z_v4]  SCOUT worker {wid} died; unassigning")
            scouts.pop(wid, None)

    counts_by_mode: dict[str, int] = {m: 0 for m in target_by_mode}
    for sc in scouts.values():
        counts_by_mode[sc.mode] = counts_by_mode.get(sc.mode, 0) + 1

    wu = [u for u in workers(units) if u["type"] == worker_type
          and u["unit_id"] not in busy_workers
          and u["unit_id"] not in scouts]
    idle_drones = [u for u in wu if u["order"] in IDLE_ORDERS]
    mining_drones = [u for u in wu if u["order"] in _MIN_ORDERS]
    pool = idle_drones + mining_drones

    for mode, target_n in target_by_mode.items():
        have = counts_by_mode.get(mode, 0)
        need = target_n - have
        wps = waypoints_by_mode.get(mode, [])
        if need <= 0 or not wps or not pool:
            continue
        for i in range(min(need, len(pool))):
            w = pool.pop(0)
            wp_idx = i % len(wps)
            tgt = wps[wp_idx]
            scouts[w["unit_id"]] = Scout(
                worker_id=w["unit_id"],
                mode=mode,
                waypoint_idx=wp_idx,
                wp_started_frame=frame,
                wp_start_dist=dist_pixels(w["x"], w["y"], tgt[0], tgt[1]),
            )
            print(f"[z_v4]  SCOUT worker {w['unit_id']} mode={mode} "
                  f"-> wp {wp_idx} {tgt} from ({w['x']},{w['y']}) "
                  f"dist={dist_pixels(w['x'], w['y'], tgt[0], tgt[1]):.0f}")

    def next_wp_idx(sc: Scout, n: int) -> int:
        cur = sc.waypoint_idx
        if len(sc.blacklist) >= n:
            print(f"[z_v4]  SCOUT {sc.worker_id} [{sc.mode}] all "
                  f"{n} waypoints blacklisted; resetting blacklist")
            sc.blacklist.clear()
        for _ in range(n):
            cur = (cur + 1) % n
            if cur not in sc.blacklist:
                return cur
        return (sc.waypoint_idx + 1) % n

    def reset_scout_wp(sc: Scout, new_idx: int, w: dict, wps: list) -> None:
        sc.waypoint_idx = new_idx
        sc.wp_started_frame = frame
        sc.wp_start_dist = dist_pixels(w["x"], w["y"],
                                       wps[new_idx][0], wps[new_idx][1])
        sc.pos_history.clear()
        sc.dist_history.clear()

    scout_ids: set[int] = set()
    for wid, sc in list(scouts.items()):
        w = find_unit(units, wid)
        if w is None:
            continue
        scout_ids.add(wid)
        wps = waypoints_by_mode.get(sc.mode, [])
        if not wps:
            continue
        wx, wy = w["x"], w["y"]
        target = wps[sc.waypoint_idx]
        d = dist_pixels(wx, wy, target[0], target[1])
        age = frame - sc.wp_started_frame if sc.wp_started_frame >= 0 else 0

        sc.pos_history.append((wx, wy))
        if len(sc.pos_history) > STUCK_WINDOW:
            sc.pos_history.pop(0)
        sc.dist_history.append(d)
        if len(sc.dist_history) > PROGRESS_WINDOW:
            sc.dist_history.pop(0)

        dd_recent = ""
        if len(sc.dist_history) >= 2:
            dd_recent = f" dd={sc.dist_history[0] - sc.dist_history[-1]:+.0f}"
        print(f"[z_v4/SCOUT] {wid}[{sc.mode}] wp={sc.waypoint_idx} "
              f"tgt={target} pos=({wx},{wy}) "
              f"d={d:.0f} start_d={sc.wp_start_dist:.0f}"
              f"{dd_recent} age={age}f "
              f"order={order_name(w['order'])} "
              f"bl={sorted(sc.blacklist)}")

        if d < ARRIVE_RADIUS:
            new_idx = next_wp_idx(sc, len(wps))
            reset_scout_wp(sc, new_idx, w, wps)
            sc.arrived_frame = frame
            print(f"[z_v4]  SCOUT {wid} [{sc.mode}] ARRIVED @{target}; "
                  f"next wp {new_idx} {wps[new_idx]}")
            try:
                nxt = wps[new_idx]
                await c.move(unit_id=wid, x=nxt[0], y=nxt[1])
            except Exception as e:
                print(f"[z_v4]  scout move error: {e}")
            continue

        stuck_reason = None
        if len(sc.pos_history) >= STUCK_WINDOW:
            xs = [p[0] for p in sc.pos_history]
            ys = [p[1] for p in sc.pos_history]
            span = max(max(xs) - min(xs), max(ys) - min(ys))
            if span < STUCK_RADIUS_PX:
                stuck_reason = f"bbox_span={span:.0f}<{STUCK_RADIUS_PX}"

        if stuck_reason is None and len(sc.dist_history) >= PROGRESS_WINDOW:
            d_delta = sc.dist_history[0] - sc.dist_history[-1]
            if d_delta < PROGRESS_MIN_DELTA_PX:
                stuck_reason = (f"progress={d_delta:+.0f}<"
                                f"{PROGRESS_MIN_DELTA_PX} over "
                                f"{PROGRESS_WINDOW}t")

        if stuck_reason is None and age > STUCK_TIMEOUT_FRAMES:
            stuck_reason = f"timeout={age}f>{STUCK_TIMEOUT_FRAMES}"

        if stuck_reason is not None:
            print(f"[z_v4]  SCOUT {wid} [{sc.mode}] STUCK near "
                  f"({wx},{wy}) wp {sc.waypoint_idx}={target}; "
                  f"reason={stuck_reason}; blacklisting.")
            sc.blacklist.add(sc.waypoint_idx)
            new_idx = next_wp_idx(sc, len(wps))
            reset_scout_wp(sc, new_idx, w, wps)
            try:
                nxt = wps[new_idx]
                await c.move(unit_id=wid, x=nxt[0], y=nxt[1])
                print(f"[z_v4]  SCOUT {wid} [{sc.mode}] -> wp {new_idx} "
                      f"{nxt} (blacklist size {len(sc.blacklist)})")
            except Exception as e:
                print(f"[z_v4]  scout skip-move error: {e}")
        else:
            order_name_str = order_name(w["order"])
            if order_name_str not in ("Move", "MoveToAttack", "AttackMove"):
                try:
                    await c.move(unit_id=wid, x=target[0], y=target[1])
                except Exception as e:
                    print(f"[z_v4]  scout re-move error: {e}")

    for e in obs.get("enemies", []):
        if e.get("building") and e["unit_id"] not in known_enemies:
            known_enemies[e["unit_id"]] = KnownEnemy(
                unit_id=e["unit_id"], type_id=e["type"],
                x=e["x"], y=e["y"], first_seen_frame=frame)
            print(f"[z_v4]  SCOUT SPOTTED enemy building "
                  f"{unit_type_name(e['type'])} @({e['x']},{e['y']}) "
                  f"unit_id={e['unit_id']}")

    for n in obs.get("neutrals", []):
        uid = n["unit_id"]
        if uid in known_resources:
            continue
        if n["type"] in (176, 177, 178, 188):
            known_resources[uid] = (n["type"], n["x"], n["y"])
            dist_home = dist_pixels(n["x"], n["y"], home_x, home_y)
            if dist_home > 1500:
                kind = "geyser" if n["type"] == 188 else "mineral"
                print(f"[z_v4]  SCOUT SPOTTED {kind} @({n['x']},{n['y']}) "
                      f"tile=({n['x']//32},{n['y']//32}) "
                      f"dist_home={dist_home:.0f} unit_id={uid}")

    return scout_ids


# --------------------------------------------------------------------
# Building anchor (Hatchery-centric spread).
# --------------------------------------------------------------------

_ANCHOR_STRATEGIES = ["toward_center", "furthest_own", "toward_center"]


def pick_anchor(units: list[dict], own_type_id: int,
                strategy_idx: int,
                home_x: int, home_y: int,
                map_w: int, map_h: int) -> tuple[int, int] | None:
    strat = _ANCHOR_STRATEGIES[strategy_idx % len(_ANCHOR_STRATEGIES)]
    # For Zerg, "own_type_id" is Hatchery; Lair (132) and Hive (133) are
    # tier-morphs of the same base so include them as anchors too.
    LAIR = UNIT_TYPES_BY_NAME["Zerg_Lair"]
    HIVE = UNIT_TYPES_BY_NAME["Zerg_Hive"]
    hatch_types = {own_type_id, LAIR, HIVE}
    cands = [u for u in units if u["type"] in hatch_types
             and u.get("completed") is True]
    if strat == "nearest_own" and cands:
        c = min(cands, key=lambda u: dist_pixels(u["x"], u["y"],
                                                 home_x, home_y))
        return (c["x"], c["y"])
    if strat == "furthest_own" and cands:
        c = max(cands, key=lambda u: dist_pixels(u["x"], u["y"],
                                                 home_x, home_y))
        return (c["x"], c["y"])
    cx, cy = map_w // 2, map_h // 2
    t = random.uniform(0.2, 0.9)
    ax = int(home_x + t * (cx - home_x))
    ay = int(home_y + t * (cy - home_y))
    ax += random.randint(-160, 160)
    ay += random.randint(-160, 160)
    return (ax, ay)


# --------------------------------------------------------------------
# Expansion: same shape as p_v4 but a Zerg "expansion" is a Hatchery.
# --------------------------------------------------------------------

CLUSTER_MERGE_PX = 400
MIN_EXPANSION_DIST_PX = 1000


def cluster_resources(known_resources: dict[int, tuple[int, int, int]]
                      ) -> list[tuple[int, int, int, int]]:
    pts = list(known_resources.values())
    if not pts:
        return []
    parent = list(range(len(pts)))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    def union(a, b):
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[ra] = rb

    for i in range(len(pts)):
        for j in range(i + 1, len(pts)):
            if dist_pixels(pts[i][1], pts[i][2],
                           pts[j][1], pts[j][2]) < CLUSTER_MERGE_PX:
                union(i, j)

    groups: dict[int, list[tuple[int, int, int]]] = {}
    for i, p in enumerate(pts):
        groups.setdefault(find(i), []).append(p)

    clusters = []
    for members in groups.values():
        if not members:
            continue
        xs = [m[1] for m in members]
        ys = [m[2] for m in members]
        cx, cy = sum(xs) // len(xs), sum(ys) // len(ys)
        n_min = sum(1 for m in members if m[0] in (176, 177, 178))
        n_gas = sum(1 for m in members if m[0] == 188)
        clusters.append((cx, cy, n_min, n_gas))
    clusters.sort(key=lambda c: c[2], reverse=True)
    return clusters


def pick_expansion_site(known_resources, own_hatcheries,
                        pending_expansion_pts):
    if not known_resources or not own_hatcheries:
        return None
    clusters = cluster_resources(known_resources)
    for cx, cy, n_min, n_gas in clusters:
        if n_min < 4:
            continue
        too_close = False
        for h in own_hatcheries:
            if dist_pixels(cx, cy, h["x"], h["y"]) < MIN_EXPANSION_DIST_PX:
                too_close = True
                break
        if too_close:
            continue
        for px, py in pending_expansion_pts:
            if dist_pixels(cx, cy, px, py) < MIN_EXPANSION_DIST_PX:
                too_close = True
                break
        if too_close:
            continue
        return (cx, cy)
    return None


async def try_expand(c: Client, obs: dict,
                     main_type: int, worker_type: int,
                     known_resources, pending_expansion_pts,
                     busy_workers: set[int]) -> Pending | None:
    r = obs["resources"]
    # Hatchery costs 300 minerals (not the Nexus's 400).
    if r["minerals"] < 300:
        return None
    units = obs["units"]
    # Any hatchery/lair/hive counts as an "existing base" for
    # expansion-distance purposes.
    LAIR = UNIT_TYPES_BY_NAME["Zerg_Lair"]
    HIVE = UNIT_TYPES_BY_NAME["Zerg_Hive"]
    own_bases = [u for u in units
                 if u["type"] in (main_type, LAIR, HIVE)
                 and u.get("completed") is True]
    if not own_bases:
        return None

    site = pick_expansion_site(known_resources, own_bases,
                               pending_expansion_pts)
    if site is None:
        return None
    cx, cy = site

    cands = [u for u in workers(units)
             if u["type"] == worker_type
             and u["unit_id"] not in busy_workers]
    if not cands:
        return None
    worker = min(cands, key=lambda u: dist_pixels(u["x"], u["y"], cx, cy))

    try:
        resp = await c.find_placement(
            unit_type=main_type,
            worker_unit=worker["unit_id"],
            center_x=cx, center_y=cy,
            radius_tiles=8, max_results=8)
    except Exception as e:
        print(f"[z_v4]  expand find_placement error: {e}")
        return None
    spots = resp.get("spots", [])
    if not spots:
        print(f"[z_v4]  EXPAND: no placement near cluster ({cx},{cy})")
        return None
    spot = spots[0]

    completed, ip = count_units(units, main_type)
    try:
        # Drone -> new Zerg building requires order=DroneStartBuild.
        await c.build(unit_id=worker["unit_id"],
                      unit_type=main_type,
                      tile_x=spot["tile_x"], tile_y=spot["tile_y"],
                      order=ORDER_DRONE_START_BUILD)
    except Exception as e:
        print(f"[z_v4]  expand build cmd error: {e}")
        return None

    pending_expansion_pts.add((cx, cy))
    print(f"[z_v4] FIRE  EXPAND Hatchery @cluster ({cx},{cy}) "
          f"tile=({spot['tile_x']},{spot['tile_y']}) "
          f"worker={worker['unit_id']}")
    return Pending(
        verb="build", target_type=main_type,
        issued_frame=obs["current_frame"],
        pre_min=r["minerals"], pre_gas=r["gas"],
        pre_count=completed + ip,
        cost_min=300, cost_gas=0,
        worker_id=worker["unit_id"],
    )


# --------------------------------------------------------------------
# Zerg build (Drone -> Zerg building via order=DroneStartBuild=25).
# --------------------------------------------------------------------

async def try_build_zerg(c: Client, obs: dict, spec: BuildingSpec,
                         worker_type: int, main_type: int,
                         pending_workers: set[int],
                         home_x: int, home_y: int,
                         map_w: int, map_h: int,
                         anchor_strategy_idx: int) -> Pending | None:
    r = obs["resources"]
    if r["minerals"] < spec.cost_min or r["gas"] < spec.cost_gas:
        return None
    units = obs["units"]
    cands = [u for u in workers(units)
             if u["type"] == worker_type
             and u["unit_id"] not in pending_workers]
    if not cands:
        return None
    worker = cands[0]

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
        kwargs["center_x"] = g["x"]; kwargs["center_y"] = g["y"]
        kwargs["radius_tiles"] = 3
    else:
        # For all other Zerg buildings, anchor around own Hatch/Lair/Hive.
        anchor_pt = pick_anchor(units, main_type,
                                anchor_strategy_idx, home_x, home_y,
                                map_w, map_h)
        if anchor_pt is not None:
            kwargs["center_x"], kwargs["center_y"] = anchor_pt

    try:
        resp = await c.find_placement(**kwargs)
    except Exception as e:
        print(f"[z_v4]  find_placement error "
              f"{unit_type_name(spec.type_id)}: {e}")
        return None
    spots = resp.get("spots", [])
    if not spots:
        return None
    spot = spots[0]

    completed, ip = count_units(units, spec.type_id)
    try:
        await c.build(unit_id=worker["unit_id"], unit_type=spec.type_id,
                      tile_x=spot["tile_x"], tile_y=spot["tile_y"],
                      order=ORDER_DRONE_START_BUILD)
    except Exception as e:
        print(f"[z_v4]  build cmd error "
              f"{unit_type_name(spec.type_id)}: {e}")
        return None
    return Pending(
        verb="build", target_type=spec.type_id,
        issued_frame=obs["current_frame"],
        pre_min=r["minerals"], pre_gas=r["gas"],
        pre_count=completed + ip,
        cost_min=spec.cost_min, cost_gas=spec.cost_gas,
        worker_id=worker["unit_id"],
    )


# --------------------------------------------------------------------
# Zerg morph unit (Larva -> unit_type via c.morph).
# --------------------------------------------------------------------

async def try_morph_unit(c: Client, obs: dict, unit_type: int,
                         cost_min: int, cost_gas: int) -> Pending | None:
    r = obs["resources"]
    if r["minerals"] < cost_min or r["gas"] < cost_gas:
        return None
    lv = idle_larvae(obs["units"])
    if not lv:
        return None
    larva = lv[0]
    completed, ip = count_units(obs["units"], unit_type)
    try:
        await c.morph(unit_id=larva["unit_id"], unit_type=unit_type)
    except Exception as e:
        print(f"[z_v4]  morph error: {e}")
        return None
    return Pending(
        verb="morph", target_type=unit_type,
        issued_frame=obs["current_frame"],
        pre_min=r["minerals"], pre_gas=r["gas"],
        pre_count=completed + ip,
        cost_min=cost_min, cost_gas=cost_gas,
    )


# --------------------------------------------------------------------
# Zerg building tier morph (Hatch -> Lair -> Hive, Creep_Colony ->
# Sunken/Spore, Spire -> Greater_Spire).
# --------------------------------------------------------------------

async def try_morph_building(c: Client, obs: dict,
                             source_type: int, target_type: int,
                             cost_min: int, cost_gas: int
                             ) -> Pending | None:
    r = obs["resources"]
    if r["minerals"] < cost_min or r["gas"] < cost_gas:
        return None
    sources = own_of_type(obs["units"], source_type)
    if not sources:
        return None
    src = sources[0]
    completed, ip = count_units(obs["units"], target_type)
    try:
        await c.morph_building(unit_id=src["unit_id"],
                               unit_type=target_type)
    except Exception as e:
        print(f"[z_v4]  morph_building error: {e}")
        return None
    return Pending(
        verb="morph_building", target_type=target_type,
        issued_frame=obs["current_frame"],
        pre_min=r["minerals"], pre_gas=r["gas"],
        pre_count=completed + ip,
        cost_min=cost_min, cost_gas=cost_gas,
    )


# --------------------------------------------------------------------
# Upgrade phase.
# --------------------------------------------------------------------

async def try_upgrade(c: Client, obs: dict,
                      spec: UpgradeSpec,
                      fired_upgrades: set[tuple[str, int]]) -> bool:
    r = obs["resources"]
    if r["minerals"] < spec.cost_min or r["gas"] < spec.cost_gas:
        return False
    key = (spec.kind, spec.enum_id)
    if key in fired_upgrades:
        return False
    sources = own_of_type(obs["units"], spec.source_type_id)
    if not sources:
        return False
    src = sources[0]
    try:
        if spec.kind == "upgrade":
            await c.upgrade(unit_id=src["unit_id"], upgrade=spec.enum_id)
        else:
            await c.research(unit_id=src["unit_id"], tech=spec.enum_id)
        fired_upgrades.add(key)
        print(f"[z_v4] FIRE  {spec.kind}:{spec.label} @{src['unit_id']} "
              f"cost={spec.cost_min}/{spec.cost_gas}")
        return True
    except Exception as e:
        print(f"[z_v4]  {spec.kind} error: {e}")
        return False


# --------------------------------------------------------------------
# Main loop.
# --------------------------------------------------------------------

# Building-tier morph plan: for each (source, target) pair with prereqs
# implicit in the sim's unit_can_build check, the agent fires
# morph_building when it has the resources + a completed source.
# Priority order = below.
#
#   Hatchery(131) -> Lair(132)   150 min / 100 gas.  Requires Pool.
#   Lair(132)     -> Hive(133)   200 min / 150 gas.  Requires Queens_Nest.
#   Spire(141)    -> Greater_Spire(137) 100/150. Requires Hive.
_HATCH = UNIT_TYPES_BY_NAME["Zerg_Hatchery"]
_LAIR = UNIT_TYPES_BY_NAME["Zerg_Lair"]
_HIVE = UNIT_TYPES_BY_NAME["Zerg_Hive"]
_SPIRE = UNIT_TYPES_BY_NAME["Zerg_Spire"]
_GREATER_SPIRE = UNIT_TYPES_BY_NAME["Zerg_Greater_Spire"]

TIER_MORPHS = [
    # (source_type, target_type, cost_min, cost_gas, description)
    (_HATCH,  _LAIR,          150, 100, "Hatchery->Lair"),
    (_LAIR,   _HIVE,          200, 150, "Lair->Hive"),
    (_SPIRE,  _GREATER_SPIRE, 100, 150, "Spire->GreaterSpire"),
]


async def run(c: Client, interval_sec: float,
              worker_target: int, supply_slack: int,
              overlord_target: int,
              scout_radial: int, scout_zscan: int,
              base_target: int) -> None:
    print(f"[z_v4] connected slot={c.welcome.slot} "
          f"frame={c.welcome.current_frame}")

    map_info = (await c.observe(targets=["map_info"]))["map_info"]
    map_w, map_h = map_info["width"], map_info["height"]

    race: str | None = None
    catalog_buildings: list[BuildingSpec] = []
    catalog_units: list[UnitSpec] = []
    catalog_upgrades: list[UpgradeSpec] = []
    worker_type = supply_type = main_type = 0

    pending: dict[str, Pending] = {}
    stats = Stats()

    scouts: dict[int, Scout] = {}
    known_enemies: dict[int, KnownEnemy] = {}
    known_resources: dict[int, tuple[int, int, int]] = {}
    waypoints_by_mode: dict[str, list[tuple[int, int]]] = {
        "radial": [], "zscan": [],
    }
    target_by_mode = {"radial": scout_radial, "zscan": scout_zscan}

    home_x = home_y = 0
    tgt_x = tgt_y = None

    anchor_strategy_idx = 0
    pending_expansion_pts: set[tuple[int, int]] = set()

    fired_upgrades: set[tuple[str, int]] = set()
    completed_upgrades: set[tuple[str, int]] = set()

    # Fired tier-morphs -- once we start a Hatch->Lair, don't re-fire
    # on the same Hatch (the type_id changes). Keyed by target_type_id.
    fired_tier_morphs: set[int] = set()

    move_done = False
    stop_done = False

    grace_frames = 600
    last_frame_seen = -1
    tick_frame_deltas: list[int] = []

    while True:
        obs = await c.observe(
            targets=["units", "resources", "enemies", "neutrals"])
        frame = obs["current_frame"]
        r = obs["resources"]
        units = obs["units"]

        if last_frame_seen >= 0:
            delta = frame - last_frame_seen
            if delta > 0:
                tick_frame_deltas.append(delta)
                if len(tick_frame_deltas) > 8:
                    tick_frame_deltas.pop(0)
                worst = max(tick_frame_deltas)
                grace_frames = max(600, min(2400, worst * 4))
        last_frame_seen = frame

        if race is None:
            race = guess_race(units)
            (catalog_buildings, catalog_units, catalog_upgrades,
             worker_type, supply_type, main_type) = race_catalogs(race)
            if units:
                home_x = sum(u["x"] for u in units) // len(units)
                home_y = sum(u["y"] for u in units) // len(units)
            waypoints_by_mode["radial"] = radial_waypoints(
                home_x, home_y, map_w, map_h, n=8)
            waypoints_by_mode["zscan"] = zscan_waypoints(map_w, map_h)
            print(f"[z_v4] race={race} home=({home_x},{home_y}) "
                  f"map={map_w}x{map_h}")
            print(f"[z_v4] radial wps: {waypoints_by_mode['radial']}")
            print(f"[z_v4] zscan wps: {len(waypoints_by_mode['zscan'])} points")

        verify_pending(pending, obs, stats, grace_frames)

        obs_upgrades = r.get("upgrades", {})
        obs_tech = set(r.get("tech", []))
        for spec in catalog_upgrades:
            key = (spec.kind, spec.enum_id)
            if key in completed_upgrades:
                continue
            if spec.kind == "upgrade":
                lvl = obs_upgrades.get(str(spec.enum_id), 0)
                if lvl > 0:
                    completed_upgrades.add(key)
                    print(f"[z_v4] TOOK  upgrade:{spec.label} (level {lvl})")
            else:
                if spec.enum_id in obs_tech:
                    completed_upgrades.add(key)
                    print(f"[z_v4] TOOK  research:{spec.label}")

        # Also observe completed tier-morphs (Lair present => Hatch->Lair done).
        if own_of_type(units, _LAIR): fired_tier_morphs.add(_LAIR)
        if own_of_type(units, _HIVE): fired_tier_morphs.add(_HIVE)
        if own_of_type(units, _GREATER_SPIRE):
            fired_tier_morphs.add(_GREATER_SPIRE)

        if known_enemies:
            e = min(known_enemies.values(),
                    key=lambda k: dist_pixels(home_x, home_y, k.x, k.y))
            tgt_x, tgt_y = e.x, e.y
        else:
            tgt_x = map_w - home_x
            tgt_y = map_h - home_y

        n_workers = len(workers(units))
        n_combat = len(combat_units(units))
        n_bldgs = len(buildings(units))
        b_types = sum(1 for s in catalog_buildings
                      if count_units(units, s.type_id)[0] > 0
                      or count_units(units, s.type_id)[1] > 0)
        u_types = sum(1 for s in catalog_units
                      if count_units(units, s.type_id)[0] > 0
                      or count_units(units, s.type_id)[1] > 0)
        ovl_c, ovl_ip = count_units(units, supply_type)
        n_upg_inprog = len(r.get("upgrading", {})) + len(r.get("researching", []))
        # Bases = Hatchery + Lair + Hive.
        base_c = sum(1 for u in units
                     if u["type"] in (_HATCH, _LAIR, _HIVE)
                     and u.get("completed") is True)
        base_ip = sum(1 for u in units
                      if u["type"] in (_HATCH, _LAIR, _HIVE)
                      and not u.get("completed", False))
        larva_c = sum(1 for u in units if u["type"] == _LARVA)
        egg_c = count_eggs(units)
        print(f"[z_v4] f={frame} min={r['minerals']} gas={r['gas']} "
              f"sup={r['supply_used']}/{r['supply_max']} "
              f"workers={n_workers}/{worker_target} "
              f"bases={base_c}(+{base_ip})/{base_target} "
              f"ovl={ovl_c}(+{ovl_ip})/{overlord_target} "
              f"larva={larva_c} eggs={egg_c} "
              f"combat={n_combat} bldgs={n_bldgs} "
              f"btypes={b_types}/{len(catalog_buildings)} "
              f"utypes={u_types}/{len(catalog_units)} "
              f"upg={len(completed_upgrades)}(+{n_upg_inprog})/{len(catalog_upgrades)} "
              f"scouts=R{sum(1 for s in scouts.values() if s.mode == 'radial')}/"
              f"Z{sum(1 for s in scouts.values() if s.mode == 'zscan')} "
              f"enemies={len(known_enemies)} "
              f"pending={len(pending)}")

        pending_workers = {p.worker_id for p in pending.values()
                           if p.worker_id is not None}

        # ---- Priority 0.5: scouting.
        scout_ids = await phase_scout(
            c, obs, worker_type, scouts, waypoints_by_mode,
            known_enemies, known_resources,
            target_by_mode=target_by_mode,
            busy_workers=pending_workers,
            home_x=home_x, home_y=home_y)
        pending_workers |= scout_ids

        # ---- Priority 1: mining.
        mining_now = await phase_mine(c, obs, worker_type,
                                      pending_workers, target_gas_workers=3)
        pending_workers |= mining_now

        # ---- Budget reservation ----
        budget = {"min": r["minerals"], "gas": r["gas"]}
        def reserve(cm, cg):
            if budget["min"] < cm or budget["gas"] < cg:
                return False
            budget["min"] -= cm; budget["gas"] -= cg
            return True

        # ---- Priority 2: supply (Overlord). Only fire when we're
        # actually running short. Overlord costs 100m and each provides
        # 8 supply.  Fire when supply_gap < supply_slack AND we're not
        # already morphing one.
        supply_gap = r["supply_max"] - r["supply_used"]
        want_overlord = (
            (supply_gap < supply_slack
             and ovl_c + ovl_ip < overlord_target)
        )
        if want_overlord and f"morph:{supply_type}" not in pending:
            if reserve(100, 0):
                p = await try_morph_unit(c, obs, supply_type, 100, 0)
                if p is not None:
                    pending[f"morph:{supply_type}"] = p
                    print(f"[z_v4] FIRE  morph:Overlord "
                          f"({ovl_c + ovl_ip + 1}/{overlord_target}) "
                          f"gap={supply_gap}")
                else:
                    budget["min"] += 100

        # ---- Priority 3: workers (Drone). Morph Larvae into Drones
        # until we hit worker_target.
        if n_workers < worker_target and f"morph:{worker_type}" not in pending:
            if reserve(50, 0):
                p = await try_morph_unit(c, obs, worker_type, 50, 0)
                if p is not None:
                    pending[f"morph:{worker_type}"] = p
                    print(f"[z_v4] FIRE  morph:Drone "
                          f"({n_workers + 1}/{worker_target})")
                else:
                    budget["min"] += 50

        # ---- Priority 4: gas structure (Extractor).
        gas_bld = UNIT_TYPES_BY_NAME["Zerg_Extractor"]
        gas_c, gas_ip = count_units(units, gas_bld)
        if gas_c + gas_ip == 0 and f"build:{gas_bld}" not in pending:
            if reserve(50, 0):
                p = await try_build_zerg(
                    c, obs, BuildingSpec(gas_bld, 50, 0, "geyser"),
                    worker_type, main_type, pending_workers,
                    home_x, home_y, map_w, map_h, anchor_strategy_idx)
                if p is not None:
                    pending[f"build:{gas_bld}"] = p
                    pending_workers.add(p.worker_id)
                    print(f"[z_v4] FIRE  build:Extractor")
                else:
                    budget["min"] += 50

        # ---- Priority 4.5: expansion (new Hatchery).
        # Same shape as p_agent_v4 but bases include Lair+Hive.
        nx_c, nx_ip = base_c, base_ip
        nx_key = f"build:{main_type}"
        if nx_key not in pending:
            pending_expansion_pts.clear()
        if (nx_c + nx_ip < base_target and nx_key not in pending
                and len(known_resources) >= 4):
            if reserve(300, 0):
                p = await try_expand(
                    c, obs, main_type, worker_type,
                    known_resources, pending_expansion_pts,
                    pending_workers)
                if p is not None:
                    pending[nx_key] = p
                    pending_workers.add(p.worker_id)
                else:
                    budget["min"] += 300

        # ---- Priority 5: catalog buildings.
        catalog_build_this_tick = 0
        for spec in catalog_buildings:
            key = f"build:{spec.type_id}"
            if key in pending: continue
            completed, ip = count_units(units, spec.type_id)
            if completed + ip > 0: continue
            if catalog_build_this_tick >= 1: break
            if not reserve(spec.cost_min, spec.cost_gas): continue
            p = await try_build_zerg(c, obs, spec, worker_type, main_type,
                                     pending_workers,
                                     home_x, home_y, map_w, map_h,
                                     anchor_strategy_idx)
            anchor_strategy_idx += 1
            if p is not None:
                pending[key] = p
                pending_workers.add(p.worker_id)
                print(f"[z_v4] FIRE  build:{unit_type_name(spec.type_id)}")
                catalog_build_this_tick += 1
            else:
                budget["min"] += spec.cost_min
                budget["gas"] += spec.cost_gas

        # ---- Priority 5.5: Zerg tier building morphs
        # (Hatch->Lair->Hive->Spire->GreaterSpire etc.).
        for src_type, tgt_type, cm, cg, label in TIER_MORPHS:
            key = f"morph_building:{tgt_type}"
            if tgt_type in fired_tier_morphs: continue
            if key in pending: continue
            # Must have a completed source AND not already having
            # any of the target type (unlikely but guards against
            # firing on an already-morphed base).
            src_c, _ = count_units(units, src_type)
            if src_c <= 0: continue
            tgt_c, tgt_ip = count_units(units, tgt_type)
            if tgt_c + tgt_ip > 0:
                fired_tier_morphs.add(tgt_type)
                continue
            if not reserve(cm, cg): continue
            p = await try_morph_building(c, obs, src_type, tgt_type, cm, cg)
            if p is not None:
                pending[key] = p
                fired_tier_morphs.add(tgt_type)
                print(f"[z_v4] FIRE  morph_building:{label}")
            else:
                budget["min"] += cm
                budget["gas"] += cg

        # ---- Priority 6: catalog units (Zerg morphs from Larva).
        # Slightly higher per-tick cap than p_agent_v4's 6 because
        # every Zerg tick has a Larva pool that can produce multiple
        # different unit types in parallel.
        catalog_train_this_tick = 0
        CATALOG_TRAIN_PER_TICK = 8
        for spec in catalog_units:
            key = f"morph:{spec.type_id}"
            if key in pending: continue
            completed, ip = count_units(units, spec.type_id)
            if completed + ip >= spec.target_count: continue
            if catalog_train_this_tick >= CATALOG_TRAIN_PER_TICK: break
            if not reserve(spec.cost_min, spec.cost_gas): continue
            p = await try_morph_unit(c, obs, spec.type_id,
                                     spec.cost_min, spec.cost_gas)
            if p is not None:
                pending[key] = p
                print(f"[z_v4] FIRE  morph:{unit_type_name(spec.type_id)} "
                      f"({completed + ip + 1}/{spec.target_count})")
                catalog_train_this_tick += 1
            else:
                budget["min"] += spec.cost_min
                budget["gas"] += spec.cost_gas

        # ---- Priority 7: upgrades.
        upgrade_this_tick = 0
        for spec in catalog_upgrades:
            if (spec.kind, spec.enum_id) in fired_upgrades: continue
            if upgrade_this_tick >= 1: break
            if not reserve(spec.cost_min, spec.cost_gas): continue
            fired = await try_upgrade(c, obs, spec, fired_upgrades)
            if fired:
                upgrade_this_tick += 1
            else:
                budget["min"] += spec.cost_min
                budget["gas"] += spec.cost_gas

        # ---- Priority 7.5: FLAGSHIP -- Larva keepup.
        # For every idle own Larva, pick the currently-most-needed
        # unit type and morph. Rate limit: up to 4 morphs per tick so
        # we don't yank every mineral in one tick.
        LARVA_KEEPUP_CAP = 4
        keepup_fires = 0
        # Recompute idle larvae; may have shrunk if any were consumed
        # earlier this tick by other passes.
        lv_now = idle_larvae(units)
        # Filter to ones NOT already consumed by earlier passes -- easy
        # heuristic: skip larva ids already recorded in a pending morph
        # (we track that via pending's worker_id when set). Actually,
        # try_morph_unit doesn't record worker_id, so instead we just
        # trust that the sim consumes a Larva atomically and the
        # observation will reflect it next tick. Rate-limit is enough.
        for lv in lv_now:
            if keepup_fires >= LARVA_KEEPUP_CAP: break
            # Decide what to morph.
            # 1. Overlord if supply is very tight (< supply_slack/2).
            # 2. Drone if worker_target not yet met.
            # 3. Zergling if pool exists.
            # 4. Hydralisk if den exists.
            # 5. Mutalisk if spire exists (and target_count not met).
            # If none of those apply, skip -- rather have an idle Larva
            # than misallocate resources into an unused type.
            supply_gap = r["supply_max"] - r["supply_used"]
            choice = None
            if (supply_gap < supply_slack // 2
                and ovl_c + ovl_ip < overlord_target
                and budget["min"] >= 100
                and f"morph:{supply_type}" not in pending):
                choice = (supply_type, 100, 0, "Overlord")
            elif (n_workers < worker_target
                  and budget["min"] >= 50
                  and f"morph:{worker_type}" not in pending):
                choice = (worker_type, 50, 0, "Drone")
            else:
                # Cheap combat first, then upgrade to tier2/3 as available.
                pool_c, _ = count_units(units,
                                        UNIT_TYPES_BY_NAME["Zerg_Spawning_Pool"])
                den_c, _ = count_units(units,
                                       UNIT_TYPES_BY_NAME["Zerg_Hydralisk_Den"])
                spire_c, _ = count_units(units, _SPIRE)
                zling_c = count_units(units,
                                      UNIT_TYPES_BY_NAME["Zerg_Zergling"])
                zling_total = zling_c[0] + zling_c[1]
                hydra_c = count_units(units,
                                      UNIT_TYPES_BY_NAME["Zerg_Hydralisk"])
                hydra_total = hydra_c[0] + hydra_c[1]
                muta_c = count_units(units,
                                     UNIT_TYPES_BY_NAME["Zerg_Mutalisk"])
                muta_total = muta_c[0] + muta_c[1]
                if pool_c > 0 and zling_total < 16 and budget["min"] >= 50:
                    choice = (UNIT_TYPES_BY_NAME["Zerg_Zergling"],
                              50, 0, "Zergling")
                elif (den_c > 0 and hydra_total < 8
                      and budget["min"] >= 75 and budget["gas"] >= 25):
                    choice = (UNIT_TYPES_BY_NAME["Zerg_Hydralisk"],
                              75, 25, "Hydralisk")
                elif (spire_c > 0 and muta_total < 4
                      and budget["min"] >= 100 and budget["gas"] >= 100):
                    choice = (UNIT_TYPES_BY_NAME["Zerg_Mutalisk"],
                              100, 100, "Mutalisk")
            if choice is None:
                continue
            unit_type, cm, cg, name = choice
            if not reserve(cm, cg):
                continue
            try:
                await c.morph(unit_id=lv["unit_id"], unit_type=unit_type)
                print(f"[z_v4] FIRE  larva-keepup morph {lv['unit_id']} "
                      f"-> {name}")
                keepup_fires += 1
                # Track counters so we don't over-fire in the same tick.
                if unit_type == worker_type: n_workers += 1
                if unit_type == supply_type: ovl_ip += 1
            except Exception as e:
                print(f"[z_v4]  larva-keepup morph error: {e}")
                budget["min"] += cm
                budget["gas"] += cg

        # ---- Priority 8: attack.
        for u in combat_units(units):
            if u["order"] not in IDLE_ORDERS: continue
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
                print(f"[z_v4]  attack error: {e}")

        # ---- Priority 9: coverage verbs (move + stop).
        if not move_done:
            idle = [u for u in workers(units)
                    if u["order"] in IDLE_ORDERS
                    and u["unit_id"] not in pending_workers]
            if idle:
                w = idle[0]
                dst_x = home_x + random.randint(-200, 200)
                dst_y = home_y + random.randint(-200, 200)
                try:
                    await c.move(unit_id=w["unit_id"], x=dst_x, y=dst_y)
                    move_done = True
                except Exception as e:
                    print(f"[z_v4]  cover move error: {e}")
        if move_done and not stop_done:
            cands = [u for u in units if u["order"] not in IDLE_ORDERS
                     and not u.get("building")]
            if cands:
                try:
                    await c.stop(unit_id=cands[0]["unit_id"])
                    stop_done = True
                except Exception as e:
                    print(f"[z_v4]  cover stop error: {e}")

        await asyncio.sleep(interval_sec)


async def main(api_key, host, port, interval_sec, worker_target,
               supply_slack, overlord_target,
               scout_radial, scout_zscan, base_target):
    async with Client(api_key=api_key, host=host, port=port) as c:
        await run(c, interval_sec, worker_target, supply_slack,
                  overlord_target,
                  scout_radial, scout_zscan, base_target)


def entrypoint() -> None:
    p = argparse.ArgumentParser(
        prog="python3 -m python_agent.agents.z_agent_v4",
        description=("Zerg counterpart of p_agent_v4 / t_agent_v4. "
                     "v3-derived infrastructure + Larva keepup flagship."))
    p.add_argument("api_key")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=6113)
    p.add_argument("--interval-sec", type=float, default=1.5)
    p.add_argument("--worker-target", type=int, default=32)
    p.add_argument("--supply-slack", type=int, default=6,
                   help="build Overlord when supply_gap < this")
    p.add_argument("--overlord-target", type=int, default=16,
                   help="cap on total Overlords")
    p.add_argument("--scout-radial", type=int, default=1)
    p.add_argument("--scout-zscan", type=int, default=1)
    p.add_argument("--base-target", type=int, default=4)
    args = p.parse_args()
    try:
        asyncio.run(main(args.api_key, args.host, args.port,
                         args.interval_sec, args.worker_target,
                         args.supply_slack, args.overlord_target,
                         args.scout_radial, args.scout_zscan,
                         args.base_target))
    except KeyboardInterrupt:
        print("\n[z_v4] stopped")


if __name__ == "__main__":
    entrypoint()
