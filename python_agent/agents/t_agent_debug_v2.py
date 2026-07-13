"""t_agent_debug_v2: combat-enabled sync stress-test fork.

Same as t_agent_debug_v1 (no scout, no lift, no repair, no mine,
no coverage) EXCEPT combat units now attack-move: 3/4 toward the
map center, 1/4 to a random tile (re-rolled per unit per tick).

Rationale: the 2026-07-13 peaceful soak (v1) proved sync.h is
clean on train/build/upgrade paths, so the surviving SyncBreaker
bugs must live in attack/move/kill code paths. v2 fires those
paths hard while everything else stays quiet, isolating the
divergence signal.

Changes from t_agent_debug_v1:
  - New Priority 8 combat pass: drive-to-center + 1/4 random.
  - Everything else identical to v1.

Original t_agent_v5 docstring below (for reference).

t_agent_v5: v4 + Terran-tactical actions.

Extends t_agent_v4 (SCV repair) with four new priority passes that
finally use the Terran-specific verbs (siege / unsiege / place_mine /
lift / build-addon) added to the server in commit 3e723cb. v4 could
train Siege Tanks but never sieged them; v5 does.

Inherited from v4:
  * SCV repair: for every own damaged mechanical unit or building,
    dispatch an idle SCV via the `repair` verb (Orders::Repair).
    Mechanical combat units qualify (SCV, Vulture, Tank both modes,
    Goliath, Wraith, Dropship, BC, Science Vessel, Valkyrie);
    biological infantry is skipped (auto-healed by Medics anyway).

New in v5:
  * Addon build (Pass A): Machine Shop attached to Factories,
    Control Tower to Starports, Comsat Station to Command Centers.
    Unlocks Machine-Shop-gated research (siege mode, spider mines,
    ion thrusters, charon boosters).
  * Auto-siege / auto-unsiege (Pass C): Siege Tanks in enemy range
    fire `siege`; tanks out of range unsiege (with hysteresis to
    prevent spam-toggle).
  * Vulture Spider Mine drop (Pass D): Vultures with mines
    remaining drop them along the home-to-enemy vector.
  * Building lift-to-safety (Pass E): lift-capable buildings (CC,
    Barracks, Factory, Starport, Science Facility) at <30 % HP
    with an enemy in range lift off and float near home.

Everything else is Terran-adapted from v3/v4: scouting (radial +
zscan), wider building spread (toward map center), upgrades/tech,
expansions (up to 4 bases), verbose scout logs.

Zerg still out of scope.

Usage:
    python3 -m python_agent.agents.t_agent_v5 <api_key>
"""

from __future__ import annotations

import argparse
import asyncio
import math
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
# Race catalogs (Terran).
# --------------------------------------------------------------------

@dataclass
class BuildingSpec:
    type_id: int
    cost_min: int
    cost_gas: int = 0
    # Terran anchors ("cc" = near Command Center, "geyser" = on a
    # geyser, "any" = far from home biased toward map center).
    # The Protoss "pylon" anchor still exists in the code path but no
    # Terran building specifies it -- Terran buildings don't need a
    # power source. "depot" is treated the same as "cc" (near CC).
    anchor: str = "any"          # "cc" | "depot" | "geyser" | "any"

@dataclass
class UnitSpec:
    type_id: int
    producer_type_id: int
    cost_min: int
    cost_gas: int = 0
    supply_each: int = 0
    # How many of this unit type to maintain (completed + in-progress).
    # Cheap infantry gets big targets so an army actually forms;
    # expensive/late-tier units (Battlecruiser, Ghost) stay low so we
    # still try one or two for coverage without dumping all resources
    # on them. Default 1 keeps original v2/v3 "one of each" semantics
    # for any spec that doesn't override.
    target_count: int = 1

@dataclass
class UpgradeSpec:
    # For BW's action_upgrade (id 50) OR action_research (id 48).
    # `kind` picks which server verb to use.
    kind: str                     # "upgrade" | "research"
    enum_id: int                  # UpgradeTypes int or TechTypes int
    source_type_id: int           # producer building type
    cost_min: int
    cost_gas: int
    label: str                    # human-readable, e.g. "GroundWeapons L1"


TERRAN_BUILDINGS: list[BuildingSpec] = [
    # Core production, roughly tech-order. Anchor "cc" places near
    # own Command_Center (equivalent to Protoss's "nexus"); "any"
    # places anywhere valid (biased toward center by anchor rotation
    # in try_build). Refinery must anchor on a geyser tile.
    BuildingSpec(UNIT_TYPES_BY_NAME["Terran_Barracks"],           150,   0, "cc"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Terran_Engineering_Bay"],   125,   0, "cc"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Terran_Bunker"],            100,   0, "cc"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Terran_Missile_Turret"],     75,   0, "cc"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Terran_Academy"],           150,   0, "any"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Terran_Factory"],           200, 100, "any"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Terran_Starport"],          150, 100, "any"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Terran_Science_Facility"],  100, 150, "any"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Terran_Armory"],            100,  50, "any"),
    # Addons (Comsat, Machine Shop, Control Tower) are handled by
    # the dedicated Pass A `phase_addon_build` in the main loop
    # below -- they use Orders::PlaceAddon (id 36) with the parent
    # building as the selected `unit`, not a worker. Not listed in
    # TERRAN_BUILDINGS because the standard `try_build` picks an
    # SCV and issues PlaceBuilding, which silent-rejects for addons.
    # See phase_addon_build docstring for the wire shape used.
]

TERRAN_UNITS: list[UnitSpec] = [
    # Cheap Barracks units -- large army targets.
    UnitSpec(UNIT_TYPES_BY_NAME["Terran_Marine"],
             UNIT_TYPES_BY_NAME["Terran_Barracks"],        50,  0, 1, target_count=8),
    UnitSpec(UNIT_TYPES_BY_NAME["Terran_Firebat"],
             UNIT_TYPES_BY_NAME["Terran_Barracks"],        50, 25, 1, target_count=8),
    UnitSpec(UNIT_TYPES_BY_NAME["Terran_Medic"],
             UNIT_TYPES_BY_NAME["Terran_Barracks"],        50, 25, 1, target_count=4),
    UnitSpec(UNIT_TYPES_BY_NAME["Terran_Ghost"],
             UNIT_TYPES_BY_NAME["Terran_Barracks"],        25, 75, 1, target_count=1),
    # Factory -- vehicles, medium cost.
    UnitSpec(UNIT_TYPES_BY_NAME["Terran_Vulture"],
             UNIT_TYPES_BY_NAME["Terran_Factory"],         75,  0, 2, target_count=2),
    UnitSpec(UNIT_TYPES_BY_NAME["Terran_Siege_Tank_Tank_Mode"],
             UNIT_TYPES_BY_NAME["Terran_Factory"],        150, 100, 2, target_count=2),
    UnitSpec(UNIT_TYPES_BY_NAME["Terran_Goliath"],
             UNIT_TYPES_BY_NAME["Terran_Factory"],        100,  50, 2, target_count=2),
    # Starport -- air, expensive.
    UnitSpec(UNIT_TYPES_BY_NAME["Terran_Wraith"],
             UNIT_TYPES_BY_NAME["Terran_Starport"],       150, 100, 2, target_count=2),
    UnitSpec(UNIT_TYPES_BY_NAME["Terran_Dropship"],
             UNIT_TYPES_BY_NAME["Terran_Starport"],       100, 100, 2, target_count=1),
    UnitSpec(UNIT_TYPES_BY_NAME["Terran_Valkyrie"],
             UNIT_TYPES_BY_NAME["Terran_Starport"],       250, 125, 3, target_count=1),
    UnitSpec(UNIT_TYPES_BY_NAME["Terran_Science_Vessel"],
             UNIT_TYPES_BY_NAME["Terran_Starport"],       100, 225, 2, target_count=1),
    UnitSpec(UNIT_TYPES_BY_NAME["Terran_Battlecruiser"],
             UNIT_TYPES_BY_NAME["Terran_Starport"],       400, 300, 6, target_count=1),
]

# Terran upgrade catalog. Enum values from bwenums.h:
#   UpgradeTypes: Terran_Infantry_Armor=0, Terran_Vehicle_Plating=1,
#                 Terran_Ship_Plating=2, Terran_Infantry_Weapons=7,
#                 Terran_Vehicle_Weapons=8, Terran_Ship_Weapons=9,
#                 U_238_Shells=16 (Marine range), Ion_Thrusters=17,
#                 Charon_Boosters=53 (Goliath range), Titan_Reactor=19,
#                 Moebius_Reactor=21, Apollo_Reactor=22, Colossus_Reactor=23,
#                 Caduceus_Reactor=50 (Medic energy)
#   TechTypes:    Stim_Packs=0, Lockdown=1, EMP_Shockwave=2,
#                 Spider_Mines=3, Tank_Siege_Mode=5, Irradiate=7,
#                 Yamato_Gun=8, Cloaking_Field=9, Personnel_Cloaking=10,
#                 Restoration=24, Optical_Flare=30
#
TERRAN_UPGRADES: list[UpgradeSpec] = [
    # Engineering Bay -- infantry upgrades, available on completion.
    UpgradeSpec("upgrade", 7, UNIT_TYPES_BY_NAME["Terran_Engineering_Bay"],
                100, 100, "InfantryWeapons_L1"),
    UpgradeSpec("upgrade", 0, UNIT_TYPES_BY_NAME["Terran_Engineering_Bay"],
                100, 100, "InfantryArmor_L1"),
    # Armory -- vehicle + ship upgrades.
    UpgradeSpec("upgrade", 8, UNIT_TYPES_BY_NAME["Terran_Armory"],
                100, 100, "VehicleWeapons_L1"),
    UpgradeSpec("upgrade", 1, UNIT_TYPES_BY_NAME["Terran_Armory"],
                100, 100, "VehiclePlating_L1"),
    UpgradeSpec("upgrade", 9, UNIT_TYPES_BY_NAME["Terran_Armory"],
                100, 100, "ShipWeapons_L1"),
    UpgradeSpec("upgrade", 2, UNIT_TYPES_BY_NAME["Terran_Armory"],
                150, 150, "ShipPlating_L1"),
    # Academy -- infantry techs.
    UpgradeSpec("research",  0, UNIT_TYPES_BY_NAME["Terran_Academy"],
                100, 100, "StimPacks"),
    UpgradeSpec("upgrade",  16, UNIT_TYPES_BY_NAME["Terran_Academy"],
                150, 150, "U-238_Shells"),
    UpgradeSpec("research",  24, UNIT_TYPES_BY_NAME["Terran_Academy"],
                100, 100, "Restoration"),
    UpgradeSpec("research",  30, UNIT_TYPES_BY_NAME["Terran_Academy"],
                100, 100, "OpticalFlare"),
    # Machine Shop -- vehicle techs. Prereq: Factory has attached
    # a Machine_Shop (v5 Pass A handles the attachment). Without it
    # try_upgrade silent-rejects.
    UpgradeSpec("research", 5, UNIT_TYPES_BY_NAME["Terran_Machine_Shop"],
                150, 150, "TankSiegeMode"),
    UpgradeSpec("research", 3, UNIT_TYPES_BY_NAME["Terran_Machine_Shop"],
                100, 100, "SpiderMines"),
    UpgradeSpec("upgrade", 17, UNIT_TYPES_BY_NAME["Terran_Machine_Shop"],
                100, 100, "IonThrusters"),
    UpgradeSpec("upgrade", 53, UNIT_TYPES_BY_NAME["Terran_Machine_Shop"],
                150, 150, "CharonBoosters"),
    # Control Tower -- Wraith cloak, Apollo Reactor.
    UpgradeSpec("research", 9, UNIT_TYPES_BY_NAME["Terran_Control_Tower"],
                150, 150, "CloakingField"),
    UpgradeSpec("upgrade", 22, UNIT_TYPES_BY_NAME["Terran_Control_Tower"],
                200, 200, "ApolloReactor"),
]


def race_catalogs(race: str):
    if race == "terran":
        return (TERRAN_BUILDINGS, TERRAN_UNITS, TERRAN_UPGRADES,
                UNIT_TYPES_BY_NAME["Terran_SCV"],
                UNIT_TYPES_BY_NAME["Terran_Supply_Depot"],
                UNIT_TYPES_BY_NAME["Terran_Command_Center"])
    raise SystemExit(f"[t_dbg2] race={race} not supported (this is the "
                     f"Terran agent -- use p_agent_v4 for Protoss)")


# --------------------------------------------------------------------
# Pending tracking (same as v2).
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
# Helpers (same shape as v2's).
# --------------------------------------------------------------------

def count_units(units: list[dict], type_id: int) -> tuple[int, int]:
    m = [u for u in units if u["type"] == type_id]
    return (sum(1 for u in m if u.get("completed") is True),
            sum(1 for u in m if not u.get("completed", False)))


def find_unit(units: list[dict], unit_id: int) -> dict | None:
    for u in units:
        if u["unit_id"] == unit_id:
            return u
    return None


def own_of_type(units: list[dict], type_id: int, only_complete=True) -> list[dict]:
    return [u for u in units if u["type"] == type_id
            and (not only_complete or u.get("completed") is True)]


# Terran mechanical unit types (repairable by SCV). Buildings are
# ALWAYS mechanical -- checked via the `building` status flag from the
# observation, not this set. Biological infantry (Marine, Firebat,
# Medic, Ghost) is intentionally absent: SCVs can't repair them.
MECHANICAL_UNIT_TYPES: set[int] = {
    UNIT_TYPES_BY_NAME["Terran_SCV"],
    UNIT_TYPES_BY_NAME["Terran_Vulture"],
    UNIT_TYPES_BY_NAME["Terran_Siege_Tank_Tank_Mode"],
    UNIT_TYPES_BY_NAME["Terran_Siege_Tank_Siege_Mode"],
    UNIT_TYPES_BY_NAME["Terran_Goliath"],
    UNIT_TYPES_BY_NAME["Terran_Wraith"],
    UNIT_TYPES_BY_NAME["Terran_Dropship"],
    UNIT_TYPES_BY_NAME["Terran_Battlecruiser"],
    UNIT_TYPES_BY_NAME["Terran_Science_Vessel"],
    UNIT_TYPES_BY_NAME["Terran_Valkyrie"],
}


def is_repairable(u: dict) -> bool:
    """True if the observation entry is a friendly, completed,
    damaged, mechanical unit or building. `u` must come from our
    own `units` list (the observation's `units[]` is same-player
    only, so friendliness is implicit).

    Returns False for units that are morphing, still under
    construction, or at full HP -- no point wasting SCV time.
    """
    if not u.get("completed"):
        return False
    hp = u.get("hp")
    hp_max = u.get("hp_max")
    if hp is None or hp_max is None or hp >= hp_max:
        return False
    if u.get("building"):
        return True
    return u.get("type") in MECHANICAL_UNIT_TYPES


_GAS_ORDERS = {ORDERS_BY_NAME[n] for n in
               ("MoveToGas", "WaitForGas", "HarvestGas", "ReturnGas")}
_MIN_ORDERS = {ORDERS_BY_NAME[n] for n in
               ("Harvest1", "Harvest2", "MoveToMinerals", "WaitForMinerals",
                "MiningMinerals", "ReturnMinerals")}


# --------------------------------------------------------------------
# Verify pending (same as v2 but grace scales with tick delta).
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
            print(f"[t_dbg2] TOOK  {p.label():48s} "
                  f"(count {p.pre_count}->{cur_count} after {age}f)")
            to_drop.append(key)
            continue
        if age >= grace_frames:
            stats.reject[p.label()] += 1
            n = stats.reject[p.label()]
            if n == 1 or n % 5 == 0:
                print(f"[t_dbg2] REJECT {p.label():48s} after {age}f. n={n}. "
                      f"pre min={p.pre_min} gas={p.pre_gas} count={p.pre_count}; "
                      f"now min={r['minerals']} gas={r['gas']} count={cur_count}")
            to_drop.append(key)
    for k in to_drop:
        pending.pop(k, None)


# --------------------------------------------------------------------
# Mining phase (from v2, unchanged).
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
                    print(f"[t_dbg2]  gather-gas error: {e}")

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
                print(f"[t_dbg2]  gather-min error: {e}")

    return just_assigned


# --------------------------------------------------------------------
# Scouting: state + waypoint dispatch.
# --------------------------------------------------------------------

@dataclass
class Scout:
    worker_id: int
    mode: str                    # "radial" | "zscan"
    waypoint_idx: int
    arrived_frame: int = -1
    # Rolling history of recent (x, y) positions -- last N ticks. Used
    # by the bounding-box stuck check.
    pos_history: list[tuple[int, int]] = field(default_factory=list)
    # Rolling history of dist-to-waypoint at each tick since we last
    # picked this waypoint. Used by the progress-based stuck check:
    # if distance hasn't dropped by > PROGRESS_MIN_DELTA_PX over
    # PROGRESS_WINDOW ticks, we're not converging on the target.
    dist_history: list[float] = field(default_factory=list)
    # Sim frame we most recently pointed this scout at its current
    # waypoint. Used by the absolute-timeout stuck check.
    wp_started_frame: int = -1
    # dist_pixels to current wp on the tick we set it. Used by
    # progress-based stuck (we know we've stalled if 90% of the
    # starting distance still remains).
    wp_start_dist: float = 0.0
    # Waypoint indices we've given up on this session. Skipped when
    # advancing. Cleared+retried once ALL indices are blacklisted.
    blacklist: set[int] = field(default_factory=set)


@dataclass
class KnownEnemy:
    unit_id: int
    type_id: int
    x: int
    y: int
    first_seen_frame: int


ARRIVE_RADIUS = 200          # pixels; "close enough" to a waypoint

# Three independent stuck checks -- any one triggers a blacklist.
#   (A) bounding-box: scout wiggled in a small area for a few ticks
#   (B) progress:      scout hasn't reduced its distance-to-wp much
#   (C) timeout:       scout has been trying to reach the wp too long
STUCK_RADIUS_PX = 100        # (A) if bbox span over STUCK_WINDOW ticks < this
STUCK_WINDOW = 4             #     ...it's stuck

PROGRESS_MIN_DELTA_PX = 200  # (B) if dist-to-wp reduced by less than this
PROGRESS_WINDOW = 8          #     ...over these many ticks, it's not converging

STUCK_TIMEOUT_FRAMES = 3000  # (C) if we've been on same wp > this many sim frames,
                             #     give up. ~30s at game-speed 10.


async def phase_scout(c: Client, obs: dict, worker_type: int,
                      scouts: dict[int, Scout],
                      waypoints_by_mode: dict[str, list[tuple[int, int]]],
                      known_enemies: dict[int, KnownEnemy],
                      known_resources: dict[int, tuple[int, int, int]],
                      target_by_mode: dict[str, int],
                      busy_workers: set[int],
                      home_x: int, home_y: int) -> set[int]:
    """Assign / advance scouts; harvest visibility for enemies + resources.

    Two scout modes run in parallel:
      * "radial"  -- 8-point ring around home (fast, near-base coverage).
      * "zscan"   -- serpentine sweep across the whole map (slow, full
                     coverage; ~36 waypoints for a 128x128 map).

    Each scout has a `mode` field; the worker's waypoint list comes
    from `waypoints_by_mode[scout.mode]`. Assignment tries to hit each
    mode's target count independently. Returns worker_ids on scout
    duty this tick.
    """
    units = obs["units"]
    frame = obs["current_frame"]

    # 1) Prune dead scouts.
    live_ids = {u["unit_id"] for u in units}
    for wid in list(scouts.keys()):
        if wid not in live_ids:
            print(f"[t_dbg2]  SCOUT worker {wid} died; unassigning")
            scouts.pop(wid, None)

    # 2) Assign new scouts up to each mode's target.
    counts_by_mode: dict[str, int] = {m: 0 for m in target_by_mode}
    for sc in scouts.values():
        counts_by_mode[sc.mode] = counts_by_mode.get(sc.mode, 0) + 1

    wu = [u for u in workers(units) if u["type"] == worker_type
          and u["unit_id"] not in busy_workers
          and u["unit_id"] not in scouts]
    idle_probes = [u for u in wu if u["order"] in IDLE_ORDERS]
    mining_probes = [u for u in wu if u["order"] in _MIN_ORDERS]
    pool = idle_probes + mining_probes

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
            print(f"[t_dbg2]  SCOUT worker {w['unit_id']} mode={mode} "
                  f"-> wp {wp_idx} {tgt} from ({w['x']},{w['y']}) "
                  f"dist={dist_pixels(w['x'], w['y'], tgt[0], tgt[1]):.0f}")

    # 3) Advance each scout in its own waypoint list. Skip blacklisted
    #    waypoints (indices we've decided are unreachable this game).
    def next_wp_idx(sc: Scout, n: int) -> int:
        """Advance sc.waypoint_idx past any blacklisted indices."""
        cur = sc.waypoint_idx
        # If everything is blacklisted, reset -- terrain may have
        # changed (enemy building destroyed, for example).
        if len(sc.blacklist) >= n:
            print(f"[t_dbg2]  SCOUT {sc.worker_id} [{sc.mode}] all "
                  f"{n} waypoints blacklisted; resetting blacklist")
            sc.blacklist.clear()
        # Advance, skipping blacklisted.
        for _ in range(n):
            cur = (cur + 1) % n
            if cur not in sc.blacklist:
                return cur
        # Fallback: everything is blacklisted even after reset (shouldn't
        # happen since we just cleared).
        return (sc.waypoint_idx + 1) % n

    def reset_scout_wp(sc: Scout, new_idx: int, w: dict, wps: list) -> None:
        """Set waypoint + reset tracking state so a fresh journey starts."""
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

        # Update rolling history.
        sc.pos_history.append((wx, wy))
        if len(sc.pos_history) > STUCK_WINDOW:
            sc.pos_history.pop(0)
        sc.dist_history.append(d)
        if len(sc.dist_history) > PROGRESS_WINDOW:
            sc.dist_history.pop(0)

        # Verbose per-tick dump. Lots of noise but it's the point --
        # this is the diagnostic log the user asked for.
        dd_recent = ""
        if len(sc.dist_history) >= 2:
            dd_recent = f" dd={sc.dist_history[0] - sc.dist_history[-1]:+.0f}"
        print(f"[t_v5/SCOUT] {wid}[{sc.mode}] wp={sc.waypoint_idx} "
              f"tgt={target} pos=({wx},{wy}) "
              f"d={d:.0f} start_d={sc.wp_start_dist:.0f}"
              f"{dd_recent} age={age}f "
              f"order={order_name(w['order'])} "
              f"bl={sorted(sc.blacklist)}")

        if d < ARRIVE_RADIUS:
            new_idx = next_wp_idx(sc, len(wps))
            reset_scout_wp(sc, new_idx, w, wps)
            sc.arrived_frame = frame
            print(f"[t_dbg2]  SCOUT {wid} [{sc.mode}] ARRIVED @{target}; "
                  f"next wp {new_idx} {wps[new_idx]}")
            try:
                nxt = wps[new_idx]
                await c.move(unit_id=wid, x=nxt[0], y=nxt[1])
            except Exception as e:
                print(f"[t_dbg2]  scout move error: {e}")
            continue

        # Three independent stuck checks (any triggers a blacklist).
        stuck_reason = None

        # (A) Bounding-box stuck: wiggled in tiny area for STUCK_WINDOW ticks.
        if len(sc.pos_history) >= STUCK_WINDOW:
            xs = [p[0] for p in sc.pos_history]
            ys = [p[1] for p in sc.pos_history]
            span = max(max(xs) - min(xs), max(ys) - min(ys))
            if span < STUCK_RADIUS_PX:
                stuck_reason = f"bbox_span={span:.0f}<{STUCK_RADIUS_PX}"

        # (B) Progress stuck: over PROGRESS_WINDOW ticks, distance-to-wp
        # dropped by less than PROGRESS_MIN_DELTA_PX. Catches "walking
        # in circles" / "inching along a cliff face with no net progress".
        if stuck_reason is None and len(sc.dist_history) >= PROGRESS_WINDOW:
            d_delta = sc.dist_history[0] - sc.dist_history[-1]
            if d_delta < PROGRESS_MIN_DELTA_PX:
                stuck_reason = (f"progress={d_delta:+.0f}<"
                                f"{PROGRESS_MIN_DELTA_PX} over "
                                f"{PROGRESS_WINDOW}t")

        # (C) Absolute timeout: on this wp too long, regardless of motion.
        if stuck_reason is None and age > STUCK_TIMEOUT_FRAMES:
            stuck_reason = f"timeout={age}f>{STUCK_TIMEOUT_FRAMES}"

        if stuck_reason is not None:
            print(f"[t_dbg2]  SCOUT {wid} [{sc.mode}] STUCK near "
                  f"({wx},{wy}) wp {sc.waypoint_idx}={target}; "
                  f"reason={stuck_reason}; blacklisting.")
            sc.blacklist.add(sc.waypoint_idx)
            new_idx = next_wp_idx(sc, len(wps))
            reset_scout_wp(sc, new_idx, w, wps)
            try:
                nxt = wps[new_idx]
                await c.move(unit_id=wid, x=nxt[0], y=nxt[1])
                print(f"[t_dbg2]  SCOUT {wid} [{sc.mode}] -> wp {new_idx} "
                      f"{nxt} (blacklist size {len(sc.blacklist)})")
            except Exception as e:
                print(f"[t_dbg2]  scout skip-move error: {e}")
        else:
            # Re-issue move only if the probe isn't already moving.
            order_name_str = order_name(w["order"])
            if order_name_str not in ("Move", "MoveToAttack",
                                      "AttackMove"):
                try:
                    await c.move(unit_id=wid, x=target[0], y=target[1])
                except Exception as e:
                    print(f"[t_dbg2]  scout re-move error: {e}")

    # 4) Harvest visibility -- remember enemies + off-base resources.
    for e in obs.get("enemies", []):
        if e.get("building") and e["unit_id"] not in known_enemies:
            known_enemies[e["unit_id"]] = KnownEnemy(
                unit_id=e["unit_id"], type_id=e["type"],
                x=e["x"], y=e["y"], first_seen_frame=frame)
            print(f"[t_dbg2]  SCOUT SPOTTED enemy building "
                  f"{unit_type_name(e['type'])} @({e['x']},{e['y']}) "
                  f"unit_id={e['unit_id']}")

    # Track resources far from home for future expansion planning.
    # Types: 176-178 = mineral fields; 188 = vespene geyser.
    for n in obs.get("neutrals", []):
        uid = n["unit_id"]
        if uid in known_resources:
            continue
        if n["type"] in (176, 177, 178, 188):
            known_resources[uid] = (n["type"], n["x"], n["y"])
            # Log off-base ones so we can see what our scouts have
            # actually discovered (as opposed to the home cluster,
            # which was visible on tick 1 -- not scouting news).
            dist_home = dist_pixels(n["x"], n["y"], home_x, home_y)
            if dist_home > 1500:
                kind = "geyser" if n["type"] == 188 else "mineral"
                print(f"[t_dbg2]  SCOUT SPOTTED {kind} @({n['x']},{n['y']}) "
                      f"tile=({n['x']//32},{n['y']//32}) "
                      f"dist_home={dist_home:.0f} unit_id={uid}")

    return scout_ids


# --------------------------------------------------------------------
# Building anchor strategies (wider distribution).
# --------------------------------------------------------------------

# Rotation is weighted toward map-center so a base on the map edge
# actually spreads inward. Two toward_center per one furthest_own,
# with nearest_own dropped entirely -- when the base is already
# tightly clustered around home (which it is by default), "nearest_own"
# just pins new buildings back on top of the CC and undoes any spread
# the other strategies achieved.
_ANCHOR_STRATEGIES = ["nearest_own", "nearest_own", "nearest_own"]


def pick_anchor(units: list[dict], own_type_id: int,
                strategy_idx: int,
                home_x: int, home_y: int,
                map_w: int, map_h: int) -> tuple[int, int] | None:
    """Return (x, y) anchor point for find_placement.

    Rotates through three strategies to spread buildings out:
      * nearest_own    -- an own building of `own_type_id` closest to home.
                          Keeps buildings tight around the base.
      * furthest_own   -- an own building of `own_type_id` furthest from
                          home. Keeps expanding outward.
      * toward_center  -- a random point along the vector home -> map
                          center. Corrects the bias problem where a base
                          near the map edge would get most of its
                          buildings clipped to the edge (nothing to the
                          east if home IS on the east edge). Nudges the
                          base toward the map interior over time.
    """
    strat = _ANCHOR_STRATEGIES[strategy_idx % len(_ANCHOR_STRATEGIES)]
    if strat == "nearest_own":
        cands = [u for u in units if u["type"] == own_type_id
                 and u.get("completed") is True]
        if not cands:
            return (home_x, home_y)
        c = min(cands, key=lambda u: dist_pixels(u["x"], u["y"],
                                                 home_x, home_y))
        return (c["x"], c["y"])
    if strat == "furthest_own":
        cands = [u for u in units if u["type"] == own_type_id
                 and u.get("completed") is True]
        if not cands:
            return (home_x, home_y)
        c = max(cands, key=lambda u: dist_pixels(u["x"], u["y"],
                                                 home_x, home_y))
        return (c["x"], c["y"])
    # toward_center: unreachable in t_debug (all _ANCHOR_STRATEGIES
    # entries are nearest_own) but clamped hard so any future refactor
    # can't send buildings toward the map center.
    cx, cy = map_w // 2, map_h // 2
    t = random.uniform(0.0, 0.1)
    ax = int(home_x + t * (cx - home_x))
    ay = int(home_y + t * (cy - home_y))
    ax += random.randint(-48, 48)
    ay += random.randint(-48, 48)
    return (ax, ay)


# --------------------------------------------------------------------
# Expansion: find a mineral cluster far from all existing CCs.
# --------------------------------------------------------------------

# A cluster of mineral fields is considered "the same patch" when its
# fields sit within CLUSTER_MERGE_PX of each other. BW mineral fields
# at a base sit within ~200 px, so 400 gives generous merge.
CLUSTER_MERGE_PX = 400
# Ignore mineral clusters closer than this to any of our CCs --
# they're at our existing base.
MIN_EXPANSION_DIST_PX = 1000


def cluster_resources(known_resources: dict[int, tuple[int, int, int]]
                      ) -> list[tuple[int, int, int, int]]:
    """Group known_resources into clusters by proximity.

    Returns [(centroid_x, centroid_y, mineral_count, geyser_count), ...]
    sorted by mineral_count desc. Naive O(n^2) flat clustering -- fine
    for the tens of resources we typically see per game.

    known_resources value shape: (type_id, x, y).
    Type 176/177/178 = mineral fields; 188 = vespene geyser.
    """
    pts = list(known_resources.values())
    if not pts:
        return []
    parent = list(range(len(pts)))  # union-find

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
            if dist_pixels(pts[i][1], pts[i][2], pts[j][1], pts[j][2]) < CLUSTER_MERGE_PX:
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


def pick_expansion_site(known_resources: dict[int, tuple[int, int, int]],
                        own_nexuses: list[dict],
                        pending_expansion_pts: set[tuple[int, int]]
                        ) -> tuple[int, int] | None:
    """Return (x, y) of a mineral cluster far from all our CCs.

    Skips clusters that have any CC (existing or in a pending
    expansion) within MIN_EXPANSION_DIST_PX. Prefers clusters with more
    mineral fields (already sorted by cluster_resources).
    """
    if not known_resources or not own_nexuses:
        return None
    clusters = cluster_resources(known_resources)
    for cx, cy, n_min, n_gas in clusters:
        # Reject clusters with too few mineral fields to be worthwhile.
        if n_min < 4:
            continue
        # Reject clusters near any existing CC.
        too_close = False
        for nx in own_nexuses:
            if dist_pixels(cx, cy, nx["x"], nx["y"]) < MIN_EXPANSION_DIST_PX:
                too_close = True
                break
        if too_close:
            continue
        # Reject clusters we've already reserved for a pending expansion.
        for px, py in pending_expansion_pts:
            if dist_pixels(cx, cy, px, py) < MIN_EXPANSION_DIST_PX:
                too_close = True
                break
        if too_close:
            continue
        return (cx, cy)
    return None


async def try_expand(c: Client, obs: dict,
                     nexus_type: int, worker_type: int,
                     known_resources: dict[int, tuple[int, int, int]],
                     pending_expansion_pts: set[tuple[int, int]],
                     busy_workers: set[int]) -> Pending | None:
    r = obs["resources"]
    if r["minerals"] < 400:
        return None
    units = obs["units"]
    own_nexuses = [u for u in units if u["type"] == nexus_type
                   and u.get("completed") is True]
    if not own_nexuses:
        return None  # can't expand from nothing

    site = pick_expansion_site(known_resources, own_nexuses,
                               pending_expansion_pts)
    if site is None:
        return None
    cx, cy = site

    # Nearest probe to the site that isn't already committed.
    cands = [u for u in workers(units)
             if u["type"] == worker_type
             and u["unit_id"] not in busy_workers]
    if not cands:
        return None
    worker = min(cands, key=lambda u: dist_pixels(u["x"], u["y"], cx, cy))

    try:
        resp = await c.find_placement(
            unit_type=nexus_type,
            worker_unit=worker["unit_id"],
            center_x=cx, center_y=cy,
            radius_tiles=8, max_results=8)
    except Exception as e:
        print(f"[t_dbg2]  expand find_placement error: {e}")
        return None
    spots = resp.get("spots", [])
    if not spots:
        print(f"[t_dbg2]  EXPAND: no placement near cluster ({cx},{cy})")
        return None
    spot = spots[0]

    completed, ip = count_units(units, nexus_type)
    try:
        await c.build(unit_id=worker["unit_id"],
                      unit_type=nexus_type,
                      tile_x=spot["tile_x"], tile_y=spot["tile_y"])
    except Exception as e:
        print(f"[t_dbg2]  expand build cmd error: {e}")
        return None

    pending_expansion_pts.add((cx, cy))
    print(f"[t_dbg2] FIRE  EXPAND CC @cluster ({cx},{cy}) "
          f"tile=({spot['tile_x']},{spot['tile_y']}) worker={worker['unit_id']}")
    return Pending(
        verb="build", target_type=nexus_type,
        issued_frame=obs["current_frame"],
        pre_min=r["minerals"], pre_gas=r["gas"],
        pre_count=completed + ip,
        cost_min=400, cost_gas=0,
        worker_id=worker["unit_id"],
    )


# --------------------------------------------------------------------
# try_build with anchor strategy hook.
# --------------------------------------------------------------------

async def try_build(c: Client, obs: dict, spec: BuildingSpec,
                    worker_type: int, main_type: int, supply_type: int,
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
        # For everything else, use the anchor strategy for wider spread.
        anchor_type = supply_type if spec.anchor == "pylon" else main_type
        anchor_pt = pick_anchor(units, anchor_type,
                                anchor_strategy_idx, home_x, home_y,
                                map_w, map_h)
        if anchor_pt is not None:
            kwargs["center_x"], kwargs["center_y"] = anchor_pt

    try:
        resp = await c.find_placement(**kwargs)
    except Exception as e:
        print(f"[t_dbg2]  find_placement error {unit_type_name(spec.type_id)}: {e}")
        return None
    spots = resp.get("spots", [])
    if not spots:
        return None
    spot = spots[0]

    completed, ip = count_units(units, spec.type_id)
    try:
        await c.build(unit_id=worker["unit_id"], unit_type=spec.type_id,
                      tile_x=spot["tile_x"], tile_y=spot["tile_y"])
    except Exception as e:
        print(f"[t_dbg2]  build cmd error {unit_type_name(spec.type_id)}: {e}")
        return None
    return Pending(
        verb="build", target_type=spec.type_id,
        issued_frame=obs["current_frame"],
        pre_min=r["minerals"], pre_gas=r["gas"],
        pre_count=completed + ip,
        cost_min=spec.cost_min, cost_gas=spec.cost_gas,
        worker_id=worker["unit_id"],
    )


async def try_train_worker(c, obs, worker_type, main_type, cost_min):
    r = obs["resources"]
    if r["minerals"] < cost_min:
        return None
    mains = own_of_type(obs["units"], main_type)
    if not mains:
        return None
    p = mains[0]
    completed, ip = count_units(obs["units"], worker_type)
    try:
        await c.train(unit_id=p["unit_id"], unit_type=worker_type)
    except Exception as e:
        print(f"[t_dbg2]  train worker error: {e}")
        return None
    return Pending(verb="train", target_type=worker_type,
                   issued_frame=obs["current_frame"],
                   pre_min=r["minerals"], pre_gas=r["gas"],
                   pre_count=completed + ip,
                   cost_min=cost_min, cost_gas=0)


async def try_train_unit(c, obs, spec: UnitSpec) -> Pending | None:
    r = obs["resources"]
    if r["minerals"] < spec.cost_min or r["gas"] < spec.cost_gas:
        return None
    prods = own_of_type(obs["units"], spec.producer_type_id)
    if not prods:
        return None
    p = prods[0]
    completed, ip = count_units(obs["units"], spec.type_id)
    try:
        await c.train(unit_id=p["unit_id"], unit_type=spec.type_id)
    except Exception as e:
        print(f"[t_dbg2]  train unit error: {e}")
        return None
    return Pending(verb="train", target_type=spec.type_id,
                   issued_frame=obs["current_frame"],
                   pre_min=r["minerals"], pre_gas=r["gas"],
                   pre_count=completed + ip,
                   cost_min=spec.cost_min, cost_gas=spec.cost_gas)


# --------------------------------------------------------------------
# Upgrade phase.
# --------------------------------------------------------------------

async def try_upgrade(c: Client, obs: dict,
                      spec: UpgradeSpec,
                      fired_upgrades: set[tuple[str, int]]) -> bool:
    """Fire an upgrade/research once. Returns True if fired.

    fired_upgrades tracks (kind, enum_id) pairs we've already fired so
    we don't spam the same upgrade every tick. We can't easily
    observe upgrade level from the wire, so this is a one-shot per
    game. Sim silently rejects re-fires anyway."""
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
        print(f"[t_dbg2] FIRE  {spec.kind}:{spec.label} @{src['unit_id']} "
              f"cost={spec.cost_min}/{spec.cost_gas}")
        return True
    except Exception as e:
        print(f"[t_dbg2]  {spec.kind} error: {e}")
        return False


# --------------------------------------------------------------------
# Main loop.
# --------------------------------------------------------------------

async def run(c: Client, interval_sec: float,
              worker_target: int, supply_slack: int,
              worker_train_min: int, pylon_target: int,
              scout_radial: int, scout_zscan: int,
              base_target: int) -> None:
    print(f"[t_dbg2] connected slot={c.welcome.slot} "
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

    # Scouting state -- one dict shared across modes, waypoints per mode.
    scouts: dict[int, Scout] = {}
    known_enemies: dict[int, KnownEnemy] = {}
    known_resources: dict[int, tuple[int, int, int]] = {}
    # v5 pass D: per-Vulture Spider Mine count. BW starts each
    # Vulture with 3 mines; we decrement on every place_mine fire.
    # Not synced with the sim -- if the client and sim disagree
    # (dropped fire, killed vulture, unit_id reuse), stale entries
    # just sit unused. `default 3` covers freshly-trained Vultures.
    mine_count_by_vulture: dict[int, int] = {}
    # v5 pass A: parent building unit_ids we've already tried to
    # attach an addon to. Once tried, don't retry on the same
    # parent even if the sim silent-rejected -- almost always the
    # rejection is because the addon slot next to the parent is
    # blocked by terrain / an adjacent building, and it stays
    # blocked. Retrying just spams the log with 15 REJECTs and
    # occasionally succeeds by luck. A future revision could
    # explicitly lift-and-relocate the parent to a spot with
    # empty addon slot, but for now: one shot per parent.
    addon_attempted: set[int] = set()
    waypoints_by_mode: dict[str, list[tuple[int, int]]] = {
        "radial": [], "zscan": [],
    }
    target_by_mode = {"radial": scout_radial, "zscan": scout_zscan}

    home_x = home_y = 0
    tgt_x = tgt_y = None  # attack target

    # Anchor rotation counter (increments per catalog build attempt).
    anchor_strategy_idx = 0

    # Expansion state: which cluster centroids we've already committed
    # a CC build to. Prevents re-firing on the same cluster while a
    # build is in progress.
    pending_expansion_pts: set[tuple[int, int]] = set()

    # Upgrades we've already fired.
    fired_upgrades: set[tuple[str, int]] = set()
    # Upgrades we've observed completed (level > 0 in obs["resources"]["upgrades"]
    # or tech id present in obs["resources"]["tech"]). Ground-truth from server.
    completed_upgrades: set[tuple[str, int]] = set()

    # Coverage move/stop one-shots.
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
            # Two scout patterns: radial (near-base ring, fast) + zscan
            # (serpentine full-map sweep, slow but complete).
            waypoints_by_mode["radial"] = radial_waypoints(
                home_x, home_y, map_w, map_h, n=8)
            waypoints_by_mode["zscan"] = zscan_waypoints(map_w, map_h)
            print(f"[t_dbg2] race={race} home=({home_x},{home_y}) "
                  f"map={map_w}x{map_h}")
            print(f"[t_dbg2] radial wps: {waypoints_by_mode['radial']}")
            print(f"[t_dbg2] zscan wps: {len(waypoints_by_mode['zscan'])} points "
                  f"({waypoints_by_mode['zscan'][:2]}...)")

        verify_pending(pending, obs, stats, grace_frames)

        # ---- Verify upgrades / tech via observation (server-authoritative).
        # obs["resources"]["upgrades"] = {upgrade_id: level}, level > 0 only.
        # obs["resources"]["tech"] = [tech_id, ...] for researched techs.
        # `upgrading`/`researching` are in-progress indicators (optional).
        obs_upgrades = r.get("upgrades", {})    # str-keyed by JSON
        obs_tech = set(r.get("tech", []))
        # Hoisted for lift-to-safety (Pass E) + siege (Pass C) +
        # attack (Priority 8) so all three see the same list.
        enemies_visible = obs.get("enemies", [])
        for spec in catalog_upgrades:
            key = (spec.kind, spec.enum_id)
            if key in completed_upgrades:
                continue
            if spec.kind == "upgrade":
                lvl = obs_upgrades.get(str(spec.enum_id), 0)
                if lvl > 0:
                    completed_upgrades.add(key)
                    print(f"[t_dbg2] TOOK  upgrade:{spec.label} "
                          f"(level {lvl})")
            else:  # research
                if spec.enum_id in obs_tech:
                    completed_upgrades.add(key)
                    print(f"[t_dbg2] TOOK  research:{spec.label}")

        # Compute attack target: nearest known enemy building; else fall
        # back to opposite corner (v2 behavior).
        if known_enemies:
            e = min(known_enemies.values(),
                    key=lambda k: dist_pixels(home_x, home_y, k.x, k.y))
            tgt_x, tgt_y = e.x, e.y
        else:
            tgt_x = map_w - home_x
            tgt_y = map_h - home_y

        # Status line.
        n_workers = len(workers(units))
        n_combat = len(combat_units(units))
        n_bldgs = len(buildings(units))
        b_types = sum(1 for s in catalog_buildings
                      if count_units(units, s.type_id)[0] > 0
                      or count_units(units, s.type_id)[1] > 0)
        u_types = sum(1 for s in catalog_units
                      if count_units(units, s.type_id)[0] > 0
                      or count_units(units, s.type_id)[1] > 0)
        pyl_c, pyl_ip = count_units(units, supply_type)
        n_upg_inprog = len(r.get("upgrading", {})) + len(r.get("researching", []))
        nx_completed, nx_in_progress = count_units(units, main_type)
        # SCV-repair summary: count own SCVs currently on Orders::Repair
        # (order id from ORDERS_BY_NAME) and count damaged mechanical
        # units/buildings that could use a repair.
        _SCV = UNIT_TYPES_BY_NAME["Terran_SCV"]
        _ORD_REPAIR = ORDERS_BY_NAME.get("Repair", 34)
        repairing = 0
        damaged_mech = 0
        for pu in units:
            if pu.get("type") == _SCV and pu.get("order") == _ORD_REPAIR:
                repairing += 1
            if is_repairable(pu):
                damaged_mech += 1
        print(f"[t_dbg2] f={frame} min={r['minerals']} gas={r['gas']} "
              f"sup={r['supply_used']}/{r['supply_max']} "
              f"workers={n_workers}/{worker_target} "
              f"bases={nx_completed}(+{nx_in_progress})/{base_target} "
              f"depot={pyl_c}(+{pyl_ip})/{pylon_target} "
              f"combat={n_combat} bldgs={n_bldgs} "
              f"btypes={b_types}/{len(catalog_buildings)} "
              f"utypes={u_types}/{len(catalog_units)} "
              f"upg={len(completed_upgrades)}(+{n_upg_inprog})/{len(catalog_upgrades)} "
              f"repair={repairing}/dmg={damaged_mech} "
              f"scouts=R{sum(1 for s in scouts.values() if s.mode == 'radial')}/"
              f"Z{sum(1 for s in scouts.values() if s.mode == 'zscan')} "
              f"enemies={len(known_enemies)} "
              f"resources_seen={len(known_resources)} "
              f"pending={len(pending)}")

        pending_workers = {p.worker_id for p in pending.values()
                           if p.worker_id is not None}

        # ---- Priority 0.5: scouting DISABLED for t_debug.
        scout_ids = set()  # was: await phase_scout(...)
        pending_workers |= scout_ids

        # ---- Priority 1: mining.
        mining_now = await phase_mine(c, obs, worker_type,
                                      pending_workers, target_gas_workers=3)
        pending_workers |= mining_now

        # ---- Budget reservation for build/train ----
        budget = {"min": r["minerals"], "gas": r["gas"]}
        def reserve(cm, cg):
            if budget["min"] < cm or budget["gas"] < cg:
                return False
            budget["min"] -= cm; budget["gas"] -= cg
            return True

        # ---- Priority 2: pylons.
        pyl_completed2, pyl_ip2 = count_units(units, supply_type)
        pyl_total2 = pyl_completed2 + pyl_ip2
        supply_gap = r["supply_max"] - r["supply_used"]
        want_more_pylons = (
            pyl_total2 < pylon_target
            and pyl_ip2 < 3
            and (supply_gap < supply_slack
                 or pyl_total2 < min(pylon_target, r["supply_used"] // 4 + 1))
        )
        if want_more_pylons and f"build:{supply_type}" not in pending:
            if reserve(100, 0):
                cands = [u for u in workers(units)
                         if u["type"] == worker_type
                         and u["unit_id"] not in pending_workers]
                if cands:
                    worker = cands[0]
                    anchor_pt = pick_anchor(units, main_type,
                                            anchor_strategy_idx,
                                            home_x, home_y,
                                            map_w, map_h)
                    anchor_strategy_idx += 1
                    try:
                        resp = await c.find_placement(
                            unit_type=supply_type,
                            worker_unit=worker["unit_id"],
                            center_x=anchor_pt[0], center_y=anchor_pt[1],
                            radius_tiles=15, max_results=8)
                        spots = resp.get("spots", [])
                        if spots:
                            spot = spots[0]
                            await c.build(unit_id=worker["unit_id"],
                                          unit_type=supply_type,
                                          tile_x=spot["tile_x"],
                                          tile_y=spot["tile_y"])
                            pending[f"build:{supply_type}"] = Pending(
                                verb="build", target_type=supply_type,
                                issued_frame=frame,
                                pre_min=r["minerals"], pre_gas=r["gas"],
                                pre_count=pyl_total2,
                                cost_min=100, cost_gas=0,
                                worker_id=worker["unit_id"])
                            pending_workers.add(worker["unit_id"])
                            print(f"[t_dbg2] FIRE  build:Supply_Depot "
                                  f"({pyl_total2 + 1}/{pylon_target}) "
                                  f"anchor={anchor_pt}")
                        else:
                            budget["min"] += 100  # refund
                    except Exception as e:
                        print(f"[t_dbg2]  supply-depot fire error: {e}")
                        budget["min"] += 100
                else:
                    budget["min"] += 100

        # ---- Priority 3: workers.
        if n_workers < worker_target and f"train:{worker_type}" not in pending:
            if reserve(worker_train_min, 0):
                p = await try_train_worker(c, obs, worker_type,
                                           main_type, worker_train_min)
                if p is not None:
                    pending[f"train:{worker_type}"] = p
                    print(f"[t_dbg2] FIRE  train:SCV ({n_workers + 1}/{worker_target})")
                else:
                    budget["min"] += worker_train_min

        # ---- Priority 4: gas structure.
        gas_bld = UNIT_TYPES_BY_NAME["Terran_Refinery"]
        gas_c, gas_ip = count_units(units, gas_bld)
        if gas_c + gas_ip == 0 and f"build:{gas_bld}" not in pending:
            if reserve(100, 0):
                p = await try_build(
                    c, obs, BuildingSpec(gas_bld, 100, 0, "geyser"),
                    worker_type, main_type, supply_type, pending_workers,
                    home_x, home_y, map_w, map_h, anchor_strategy_idx)
                if p is not None:
                    pending[f"build:{gas_bld}"] = p
                    pending_workers.add(p.worker_id)
                    print(f"[t_dbg2] FIRE  build:Refinery")
                else:
                    budget["min"] += 100

        # ---- Priority 4.5: expansion (new base). Higher priority than
        # catalog buildings because we want to reserve the 400 min
        # before it gets eaten by a Fleet Beacon (300) etc.
        # Prune stale pending_expansion_pts: if the pending build for
        # `main_type` cleared this tick (via count-delta verify), drop
        # the reservation.
        nx_c, nx_ip = count_units(units, main_type)
        nx_key = f"build:{main_type}"
        if nx_key not in pending:
            # No expansion in flight; clear our reservation set so a
            # completed expansion doesn't block future ones.
            pending_expansion_pts.clear()
        if (nx_c + nx_ip < base_target and nx_key not in pending
                and len(known_resources) >= 4):
            if reserve(400, 0):
                p = await try_expand(
                    c, obs, main_type, worker_type,
                    known_resources, pending_expansion_pts,
                    pending_workers)
                if p is not None:
                    pending[nx_key] = p
                    pending_workers.add(p.worker_id)
                else:
                    budget["min"] += 400  # refund

        # ---- Priority 5: catalog buildings (1 per tick).
        catalog_build_this_tick = 0
        for spec in catalog_buildings:
            key = f"build:{spec.type_id}"
            if key in pending: continue
            completed, ip = count_units(units, spec.type_id)
            if completed + ip > 0: continue
            if catalog_build_this_tick >= 1: break
            if not reserve(spec.cost_min, spec.cost_gas): continue
            p = await try_build(c, obs, spec, worker_type, main_type,
                                supply_type, pending_workers,
                                home_x, home_y, map_w, map_h,
                                anchor_strategy_idx)
            anchor_strategy_idx += 1
            if p is not None:
                pending[key] = p
                pending_workers.add(p.worker_id)
                print(f"[t_dbg2] FIRE  build:{unit_type_name(spec.type_id)}")
                catalog_build_this_tick += 1
            else:
                budget["min"] += spec.cost_min
                budget["gas"] += spec.cost_gas

        # ---- Priority 5.5: Terran addons (v5 pass A).
        # For each completed parent building without its canonical
        # addon, fire the `build` verb with the parent building's own
        # unit_id as the selection and order=36 (PlaceAddon). The sim
        # computes the addon tile from parent->addon_position; tile_x
        # /tile_y in our payload are ignored for PlaceAddon but still
        # required by the wire shape, so we pass the parent's tile.
        # Rate-limit: 1 per tick.
        # Attachments: Factory -> Machine_Shop (unlocks Siege Mode,
        # Spider Mines, Ion Thrusters, Charon Boosters). Starport ->
        # Control_Tower (unlocks Wraith cloak, Apollo Reactor). CC ->
        # Comsat_Station (unlocks Scanner Sweep -- not exercised in
        # this version, just built for coverage).
        _ADDON_ATTACHMENTS = [
            # (parent_type, addon_type, addon_min, addon_gas, addon_name)
            (UNIT_TYPES_BY_NAME["Terran_Factory"],
             UNIT_TYPES_BY_NAME["Terran_Machine_Shop"], 50, 50, "Machine_Shop"),
            (UNIT_TYPES_BY_NAME["Terran_Starport"],
             UNIT_TYPES_BY_NAME["Terran_Control_Tower"], 50, 50, "Control_Tower"),
            (UNIT_TYPES_BY_NAME["Terran_Command_Center"],
             UNIT_TYPES_BY_NAME["Terran_Comsat_Station"], 50, 50, "Comsat_Station"),
        ]
        addon_fired = False
        for parent_type, addon_type, amin, agas, aname in _ADDON_ATTACHMENTS:
            if addon_fired: break
            done, ip = count_units(units, addon_type)
            if done + ip > 0: continue
            # Pick a completed parent that hasn't been tried before.
            parents = [p for p in own_of_type(units, parent_type)
                       if p["unit_id"] not in addon_attempted]
            if not parents: continue
            if not reserve(amin, agas): continue
            parent = parents[0]
            parent_tile_x = parent["x"] // 32
            parent_tile_y = parent["y"] // 32
            try:
                await c.cmd({"verb": "build",
                             "unit": parent["unit_id"],
                             "unit_type": addon_type,
                             "tile_x": parent_tile_x,
                             "tile_y": parent_tile_y,
                             "order": 36})  # Orders::PlaceAddon
                addon_attempted.add(parent["unit_id"])
                print(f"[t_dbg2] FIRE  addon:{aname} on "
                      f"{unit_type_name(parent_type)} {parent['unit_id']}")
                addon_fired = True
            except Exception as e:
                budget["min"] += amin; budget["gas"] += agas
                print(f"[t_dbg2]  addon fire error: {e}")

        # ---- Priority 6: catalog units (up to 6 fires per tick,
        #      throttled per-type by target_count and pending grace).
        # Each UnitSpec carries target_count -- how many completed +
        # in-progress copies to maintain. Cheap Barracks infantry get
        # big targets (8) so an army actually forms; expensive
        # Battlecruiser / Ghost stay low (1) so they don't monopolise
        # gas. Pending grace keys per type_id so we don't refire the
        # same type before the sim shows the new unit in ip; different
        # types can fire concurrently up to the per-tick cap below.
        catalog_train_this_tick = 0
        CATALOG_TRAIN_PER_TICK = 6
        for spec in catalog_units:
            key = f"train:{spec.type_id}"
            if key in pending: continue
            completed, ip = count_units(units, spec.type_id)
            if completed + ip >= spec.target_count: continue
            if catalog_train_this_tick >= CATALOG_TRAIN_PER_TICK: break
            if not reserve(spec.cost_min, spec.cost_gas): continue
            p = await try_train_unit(c, obs, spec)
            if p is not None:
                pending[key] = p
                print(f"[t_dbg2] FIRE  train:{unit_type_name(spec.type_id)} "
                      f"({completed + ip + 1}/{spec.target_count})")
                catalog_train_this_tick += 1
            else:
                budget["min"] += spec.cost_min
                budget["gas"] += spec.cost_gas

        # ---- Priority 7: upgrades (1 per tick).
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

        # ---- Priority 7.4: lift-to-safety DISABLED for t_debug.
        # (v5's original pass fired `c.lift(bld, x=home, y=home)` —
        # forbidden in the stress test since it moves the building.)

        # ---- Priority 7.5: SCV repair DISABLED for t_debug.
        # (v5's original pass fired `c.repair(scv, tgt)` — this pulls
        # SCVs off the mineral line to a repair target, i.e. movement.
        # Forbidden in the stress test.)

        # ---- Priority 7.7: auto-siege / auto-unsiege DISABLED for t_debug.
        # (Siege itself is stationary, but this pass depends on visible-
        # enemy logic that would only fire when things move — noise for
        # the sync stress test. Also there are no enemies to detect since
        # neither debug agent sends units out.)

        # ---- Priority 7.8: Vulture Spider Mine drop DISABLED for t_debug.
        # (v5 places mines along the home->enemy vector, i.e. moves
        # Vultures out. Forbidden in the stress test.)

        # ---- Priority 8 (v2): drive-combat-to-center ----
        # v2 upgrade: SEND combat units out to fight. 3/4 toward the
        # map center, 1/4 to a random tile per unit per tick. Skip
        # units already on an attack order.
        cx_map, cy_map = map_w // 2, map_h // 2
        # Skip units already on an attack order. Guard/PlayerGuard
        # are BW's default idle orders (see enums.IDLE_ORDERS) —
        # they must NOT be in this set.
        _ATTACK_IN_FLIGHT = {
            ORDERS_BY_NAME.get(n) for n in (
                "AttackMove", "AttackUnit", "AttackTile",
                "AttackFixedRange",
            ) if ORDERS_BY_NAME.get(n) is not None
        }
        _attack_fires = 0
        for u in combat_units(units):
            if _attack_fires >= 12:
                break
            if u.get("order") in _ATTACK_IN_FLIGHT:
                continue
            if random.random() < 0.25:
                tx = random.randint(64, map_w - 64)
                ty = random.randint(64, map_h - 64)
            else:
                tx, ty = cx_map, cy_map
            try:
                await c.attack(unit_id=u["unit_id"],
                               target_unit=0, x=tx, y=ty)
                _attack_fires += 1
            except Exception as e:
                print(f"[t_dbg2]  attack error: {e}")

        # ---- Priority 9: coverage verbs still DISABLED in v2.

        await asyncio.sleep(interval_sec)


async def main(api_key, host, port, url, interval_sec, worker_target,
               supply_slack, worker_train_min, pylon_target,
               scout_radial, scout_zscan, base_target, action_log):
    if url:
        client_kwargs = {"api_key": api_key, "url": url}
    else:
        client_kwargs = {"api_key": api_key, "host": host, "port": port}
    if action_log:
        client_kwargs["action_log_path"] = action_log
    async with Client(**client_kwargs) as c:
        await run(c, interval_sec, worker_target, supply_slack,
                  worker_train_min, pylon_target,
                  scout_radial, scout_zscan, base_target)


def entrypoint() -> None:
    p = argparse.ArgumentParser(
        prog="python3 -m python_agent.agents.t_agent_debug_v2",
        description="t_debug: sync stress-test variant of t_agent_v5. "
                    "Spams production/build actions; NEVER issues "
                    "attack/move/lift/repair/mine.")
    p.add_argument("api_key")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=6113)
    p.add_argument("--url", default=None,
                   help="full wss://.../agent URL (overrides --host/--port); "
                        "use this to connect through the simsc ALB")
    p.add_argument("--interval-sec", type=float, default=0.1,
                   help="tick interval in seconds. t_debug fires as fast "
                        "as reasonable (default 0.1s) to stress the sync "
                        "action fan-out.")
    p.add_argument("--worker-target", type=int, default=40)
    p.add_argument("--supply-slack", type=int, default=8)
    p.add_argument("--worker-train-min", type=int, default=50)
    p.add_argument("--pylon-target", type=int, default=20)
    # scout flags kept for CLI parity but the pass is disabled.
    p.add_argument("--scout-radial", type=int, default=0,
                   help="DISABLED in t_debug (kept for CLI parity)")
    p.add_argument("--scout-zscan", type=int, default=0,
                   help="DISABLED in t_debug (kept for CLI parity)")
    p.add_argument("--base-target", type=int, default=2,
                   help="target total CC count including main base. "
                        "t_debug defaults to 2 to keep buildings clustered.")
    p.add_argument("--action-log", default=None,
                   help="file to append AGENT_ISSUE_CLIENT rows to. See "
                        "python_agent/client.py for the format.")
    args = p.parse_args()
    try:
        asyncio.run(main(args.api_key, args.host, args.port, args.url,
                         args.interval_sec, args.worker_target,
                         args.supply_slack, args.worker_train_min,
                         args.pylon_target,
                         args.scout_radial, args.scout_zscan,
                         args.base_target, args.action_log))
    except KeyboardInterrupt:
        print("\n[t_dbg2] stopped")


if __name__ == "__main__":
    entrypoint()
