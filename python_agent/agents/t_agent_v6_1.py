"""t_agent_v6_1: v6 + earlier expansion + more turrets + push mode.

v6 turtled correctly but stalled on gas: the "no expansion until
2 bunkers and 3 tanks" gate could never be satisfied because tanks
need gas and gas needs a second geyser. v6_1 fixes the deadlock and
adds a push-forward mode once the base is stable.

Changes vs. v6:
  * Expansion gate: only requires >= 1 completed Bunker (was 2
    bunkers + 3 tanks). Second base is chosen with the v6 "our-
    side" preference so it still lands NE / SE / etc. rather than
    marching straight at the enemy.
  * Turrets: `--turret-target` (default 4). Placed in a spread
    around the defense anchor AND around home so anti-air /
    anti-cloak coverage isn't a single choke point.
  * BuildingSpec target_count: Factory and Barracks get target 2
    (was 1) so a lost production building doesn't stall the army.
  * Push-forward mode: once we have >= 3 tanks at the current
    defense anchor OR the game reaches --push-after-frames
    (default 25000, ~4 min at speed 10), the anchor advances 0.15
    per interval even with an enemy in view, and off-anchor
    sieged tanks unsiege so they can catch up. Bunker/turret
    builds at the NEW anchor drag the defense line forward
    behind the tanks.

All other v6 passes are unchanged: mining-corridor guard, compact
placement (2x near_home + 1x toward_center capped at 35 %), turtle
placement priorities, SCV repair, addon build, auto-siege, lift-to-
safety.

Server verbs still unavailable (see SERVER_ASKS.md): loading marines
into a bunker.

Usage:
    python3 -m python_agent.agents.t_agent_v6_1 <api_key>

--- inherited notes from v6: ---
  * Turtle placement (`_ANCHOR_STRATEGIES` = 2x near_home + 1x
    toward_center capped at t <= 0.35, and `try_build`
    `radius_tiles` shrinks from 20 to 12). Buildings cluster
    close to the Command Center; a shorter defense arc.
  * Mining-corridor guard: `find_placement` spots that sit inside
    a 2-tile-wide band on the segment CC <-> mineral or
    CC <-> geyser are rejected. SCVs keep clean straight-line
    paths to their patches.
  * Bunker line: `--bunker-target` (default 3). Bunkers are
    placed at the *defense anchor* (see below), not near map
    center. Missile Turrets bracket them.
  * More tanks: Terran_Siege_Tank_Tank_Mode target_count goes
    from 2 to 6. Tanks that arrive at the defense anchor siege
    unconditionally, not only when an enemy is in range, so the
    line is armed before the enemy shows up.
  * Defense-first expansion: expansion (Priority 4.5) requires
    >= 2 completed Bunkers AND >= 3 completed Siege Tanks
    (either mode). Below that, resources bank for defense.
  * Defense-line step: `defense_step` int; each time all tanks
    reach the current anchor and no enemy is nearby, step forward
    along home -> enemy vector by 0.10 (max 0.60). Later bunkers
    are placed at successive anchors.
  * Second-base preference: if any enemy has been seen, prefer
    clusters on OUR side of the map (dist-to-enemy > dist-to-home).
    Falls back to v5's largest-cluster-far-from-CC logic when
    ambiguous.
  * `--defense-only`: skip Priority 8 attack. Idle combat units
    stay on the defense line instead of walking off toward
    enemy base. Default: ON (pure turtle).

Server verbs still unavailable in v6 (see SERVER_ASKS.md at repo
root): loading marines into a bunker. Marines instead hold a
200-px ring around each bunker via `move`, which keeps them in
firing range but does not actually enter the bunker.

Usage:
    python3 -m python_agent.agents.t_agent_v6 <api_key>
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
    # v6_1: how many copies of this building to maintain (completed
    # + in-progress). Default 1 preserves v6's "one of each" build
    # behaviour; production redundancy (Barracks, Factory) is set
    # to 2 so losing one under attack doesn't stall the army.
    target_count: int = 1

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
    # v6 order: production and tech first so we can train tanks;
    # Bunker/Missile_Turret are handled outside this catalog (see
    # phase_defense_line in the main loop) so they can be placed
    # at the defense anchor rather than the general "cc" anchor.
    # Anchor "cc" places near own Command_Center; "any" uses the
    # anchor rotation strategy (v6: 2x near_home + 1x toward_center
    # capped). Refinery must anchor on a geyser tile.
    BuildingSpec(UNIT_TYPES_BY_NAME["Terran_Barracks"],           150,   0, "cc", target_count=2),
    BuildingSpec(UNIT_TYPES_BY_NAME["Terran_Engineering_Bay"],   125,   0, "cc"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Terran_Factory"],           200, 100, "cc", target_count=2),
    BuildingSpec(UNIT_TYPES_BY_NAME["Terran_Academy"],           150,   0, "cc"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Terran_Armory"],            100,  50, "cc"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Terran_Starport"],          150, 100, "any"),
    BuildingSpec(UNIT_TYPES_BY_NAME["Terran_Science_Facility"],  100, 150, "any"),
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
    # v6: turtle line wants many tanks. 6 total is enough for a
    # thick siege ring and lets us step the line forward once every
    # tank has arrived at the current anchor.
    UnitSpec(UNIT_TYPES_BY_NAME["Terran_Siege_Tank_Tank_Mode"],
             UNIT_TYPES_BY_NAME["Terran_Factory"],        150, 100, 2, target_count=6),
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
    raise SystemExit(f"[t_v6_1] race={race} not supported (this is the "
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
            print(f"[t_v6_1] TOOK  {p.label():48s} "
                  f"(count {p.pre_count}->{cur_count} after {age}f)")
            to_drop.append(key)
            continue
        if age >= grace_frames:
            stats.reject[p.label()] += 1
            n = stats.reject[p.label()]
            if n == 1 or n % 5 == 0:
                print(f"[t_v6_1] REJECT {p.label():48s} after {age}f. n={n}. "
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
                    print(f"[t_v6_1]  gather-gas error: {e}")

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
                print(f"[t_v6_1]  gather-min error: {e}")

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
            print(f"[t_v6_1]  SCOUT worker {wid} died; unassigning")
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
            print(f"[t_v6_1]  SCOUT worker {w['unit_id']} mode={mode} "
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
            print(f"[t_v6_1]  SCOUT {sc.worker_id} [{sc.mode}] all "
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
        print(f"[t_v6_1/SCOUT] {wid}[{sc.mode}] wp={sc.waypoint_idx} "
              f"tgt={target} pos=({wx},{wy}) "
              f"d={d:.0f} start_d={sc.wp_start_dist:.0f}"
              f"{dd_recent} age={age}f "
              f"order={order_name(w['order'])} "
              f"bl={sorted(sc.blacklist)}")

        if d < ARRIVE_RADIUS:
            new_idx = next_wp_idx(sc, len(wps))
            reset_scout_wp(sc, new_idx, w, wps)
            sc.arrived_frame = frame
            print(f"[t_v6_1]  SCOUT {wid} [{sc.mode}] ARRIVED @{target}; "
                  f"next wp {new_idx} {wps[new_idx]}")
            try:
                nxt = wps[new_idx]
                await c.move(unit_id=wid, x=nxt[0], y=nxt[1])
            except Exception as e:
                print(f"[t_v6_1]  scout move error: {e}")
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
            print(f"[t_v6_1]  SCOUT {wid} [{sc.mode}] STUCK near "
                  f"({wx},{wy}) wp {sc.waypoint_idx}={target}; "
                  f"reason={stuck_reason}; blacklisting.")
            sc.blacklist.add(sc.waypoint_idx)
            new_idx = next_wp_idx(sc, len(wps))
            reset_scout_wp(sc, new_idx, w, wps)
            try:
                nxt = wps[new_idx]
                await c.move(unit_id=wid, x=nxt[0], y=nxt[1])
                print(f"[t_v6_1]  SCOUT {wid} [{sc.mode}] -> wp {new_idx} "
                      f"{nxt} (blacklist size {len(sc.blacklist)})")
            except Exception as e:
                print(f"[t_v6_1]  scout skip-move error: {e}")
        else:
            # Re-issue move only if the probe isn't already moving.
            order_name_str = order_name(w["order"])
            if order_name_str not in ("Move", "MoveToAttack",
                                      "AttackMove"):
                try:
                    await c.move(unit_id=wid, x=target[0], y=target[1])
                except Exception as e:
                    print(f"[t_v6_1]  scout re-move error: {e}")

    # 4) Harvest visibility -- remember enemies + off-base resources.
    for e in obs.get("enemies", []):
        if e.get("building") and e["unit_id"] not in known_enemies:
            known_enemies[e["unit_id"]] = KnownEnemy(
                unit_id=e["unit_id"], type_id=e["type"],
                x=e["x"], y=e["y"], first_seen_frame=frame)
            print(f"[t_v6_1]  SCOUT SPOTTED enemy building "
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
                print(f"[t_v6_1]  SCOUT SPOTTED {kind} @({n['x']},{n['y']}) "
                      f"tile=({n['x']//32},{n['y']//32}) "
                      f"dist_home={dist_home:.0f} unit_id={uid}")

    return scout_ids


# --------------------------------------------------------------------
# Building anchor strategies (wider distribution).
# --------------------------------------------------------------------

# v6 turtle rotation: mostly stay near home, occasional nudge
# toward the map center capped at 35 % of the way (v5 went 20..90 %).
# Result: a compact base with a short defense arc, instead of
# buildings creeping into the middle of the map where they can't
# be defended by the CC's own turret ring.
_ANCHOR_STRATEGIES = ["near_home", "near_home", "toward_center"]


def pick_anchor(units: list[dict], own_type_id: int,
                strategy_idx: int,
                home_x: int, home_y: int,
                map_w: int, map_h: int) -> tuple[int, int] | None:
    """Return (x, y) anchor point for find_placement.

    v6 rotation prioritises keeping the base compact. Two out of every
    three attempts anchor right at the CC (with a small jitter). The
    third nudges slightly toward map center, but only up to 35 % of
    the way -- enough that a corner base doesn't box itself in
    against the edge, without smearing buildings into the middle of
    the map where they can't be defended.
    """
    strat = _ANCHOR_STRATEGIES[strategy_idx % len(_ANCHOR_STRATEGIES)]
    if strat == "near_home":
        # Small jitter around the CC to spread buildings out a bit
        # rather than stacking on the same pixel.
        return (home_x + random.randint(-80, 80),
                home_y + random.randint(-80, 80))
    # toward_center: capped 15..35 % of the way from home to map center.
    cx, cy = map_w // 2, map_h // 2
    t = random.uniform(0.15, 0.35)
    ax = int(home_x + t * (cx - home_x))
    ay = int(home_y + t * (cy - home_y))
    ax += random.randint(-96, 96)
    ay += random.randint(-96, 96)
    return (ax, ay)


# --------------------------------------------------------------------
# v6 mining-corridor guard.
#
# BW SCVs walk in a mostly straight line from their base to the
# mineral field or geyser they've been assigned. If a building sits
# in that corridor the SCV has to path around it, which quietly
# tanks mining rate. This guard rejects `find_placement` spots
# whose center (in pixels) sits inside a rectangle around any
# CC <-> resource segment. Width of the rectangle is
# `CORRIDOR_HALF_WIDTH_PX` on each side of the segment.
#
# CORRIDOR_HALF_WIDTH_PX = 64 px (2 tiles) on each side. Wide enough
# that a 3x3 Factory placed on the edge of the corridor still won't
# clip the path (Factory footprint = 4 tiles wide).
# --------------------------------------------------------------------

CORRIDOR_HALF_WIDTH_PX = 64


def _point_segment_dist_px(px: float, py: float,
                           ax: float, ay: float,
                           bx: float, by: float) -> float:
    """Shortest distance in pixels from point (px,py) to segment
    (ax,ay)-(bx,by). Standard vector projection with clamping to
    the endpoints. Only used for the corridor guard, so a plain
    Python impl is fine (called a handful of times per placement)."""
    dx = bx - ax
    dy = by - ay
    if dx == 0 and dy == 0:
        return dist_pixels(int(px), int(py), int(ax), int(ay))
    t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)
    t = max(0.0, min(1.0, t))
    qx = ax + t * dx
    qy = ay + t * dy
    return dist_pixels(int(px), int(py), int(qx), int(qy))


def in_mining_corridor(spot_px_x: int, spot_px_y: int,
                       units: list[dict], neutrals: list[dict],
                       main_type: int) -> bool:
    """True if the placement spot sits inside the SCV path corridor
    between any own CC and any nearby mineral field or geyser.

    Both minerals and geysers are checked. "Nearby" means within
    900 px of the CC (roughly one screen), so we don't reject spots
    just because a mineral 3 screens away happens to project through
    them.
    """
    ccs = [u for u in units if u["type"] == main_type]
    if not ccs:
        return False
    # v6 resource types: mineral = 176/177/178, geyser = 188, refinery = 116.
    _MIN_TYPES = {176, 177, 178}
    _GAS_TYPES = {188}
    NEAR_CC_PX = 900

    for cc in ccs:
        cx, cy = cc["x"], cc["y"]
        for r in neutrals:
            rt = r.get("type")
            if rt not in _MIN_TYPES and rt not in _GAS_TYPES:
                continue
            rx, ry = r["x"], r["y"]
            if dist_pixels(cx, cy, rx, ry) > NEAR_CC_PX:
                continue
            d = _point_segment_dist_px(
                spot_px_x, spot_px_y, cx, cy, rx, ry)
            if d < CORRIDOR_HALF_WIDTH_PX:
                return True
        # Also treat own refineries as "gas endpoints" (same corridor
        # as the geyser they replaced -- the geyser vanishes from the
        # neutrals list once the Refinery is built).
        _REFINERY = UNIT_TYPES_BY_NAME["Terran_Refinery"]
        for u in units:
            if u["type"] != _REFINERY:
                continue
            if dist_pixels(cx, cy, u["x"], u["y"]) > NEAR_CC_PX:
                continue
            d = _point_segment_dist_px(
                spot_px_x, spot_px_y, cx, cy, u["x"], u["y"])
            if d < CORRIDOR_HALF_WIDTH_PX:
                return True
    return False


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
                        pending_expansion_pts: set[tuple[int, int]],
                        known_enemies: dict | None = None,
                        home_x: int = 0, home_y: int = 0,
                        ) -> tuple[int, int] | None:
    """Return (x, y) of a mineral cluster far from all our CCs.

    Skips clusters that have any CC (existing or in a pending
    expansion) within MIN_EXPANSION_DIST_PX. Prefers clusters with more
    mineral fields (already sorted by cluster_resources).

    v6: when we've seen any enemy building, prefer clusters that sit
    on OUR side of the map -- i.e. distance-to-enemy is greater than
    distance-to-home. Falls back to the v5 "biggest cluster far from
    a CC" behaviour when there's no candidate on our side. This
    stops the turtle from expanding directly into the opponent's
    front line.
    """
    if not known_resources or not own_nexuses:
        return None
    clusters = cluster_resources(known_resources)

    # Split into "on our side" and "everywhere else" once we know
    # where an enemy is. Both lists preserve the cluster_resources
    # sort (biggest first).
    our_side: list[tuple[int, int, int, int]] = []
    rest: list[tuple[int, int, int, int]] = []
    enemy_pt: tuple[int, int] | None = None
    if known_enemies:
        e = min(known_enemies.values(),
                key=lambda k: dist_pixels(home_x, home_y, k.x, k.y))
        enemy_pt = (e.x, e.y)
    for c in clusters:
        cx, cy, _, _ = c
        if enemy_pt is not None:
            d_home = dist_pixels(cx, cy, home_x, home_y)
            d_enemy = dist_pixels(cx, cy, enemy_pt[0], enemy_pt[1])
            if d_home < d_enemy:
                our_side.append(c)
            else:
                rest.append(c)
        else:
            rest.append(c)

    def _first_valid(cands: list[tuple[int, int, int, int]]
                     ) -> tuple[int, int] | None:
        for cx, cy, n_min, n_gas in cands:
            if n_min < 4:
                continue
            too_close = False
            for nx in own_nexuses:
                if dist_pixels(cx, cy, nx["x"], nx["y"]) < MIN_EXPANSION_DIST_PX:
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

    # Prefer our side; fall back to any cluster if none on our side
    # is valid (e.g. we're crammed against one edge of the map).
    return _first_valid(our_side) or _first_valid(rest)


async def try_expand(c: Client, obs: dict,
                     nexus_type: int, worker_type: int,
                     known_resources: dict[int, tuple[int, int, int]],
                     pending_expansion_pts: set[tuple[int, int]],
                     busy_workers: set[int],
                     known_enemies: dict | None = None,
                     home_x: int = 0, home_y: int = 0,
                     ) -> Pending | None:
    r = obs["resources"]
    if r["minerals"] < 400:
        return None
    units = obs["units"]
    own_nexuses = [u for u in units if u["type"] == nexus_type
                   and u.get("completed") is True]
    if not own_nexuses:
        return None  # can't expand from nothing

    site = pick_expansion_site(known_resources, own_nexuses,
                               pending_expansion_pts,
                               known_enemies=known_enemies,
                               home_x=home_x, home_y=home_y)
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
        print(f"[t_v6_1]  expand find_placement error: {e}")
        return None
    spots = resp.get("spots", [])
    if not spots:
        print(f"[t_v6_1]  EXPAND: no placement near cluster ({cx},{cy})")
        return None
    spot = spots[0]

    completed, ip = count_units(units, nexus_type)
    try:
        await c.build(unit_id=worker["unit_id"],
                      unit_type=nexus_type,
                      tile_x=spot["tile_x"], tile_y=spot["tile_y"])
    except Exception as e:
        print(f"[t_v6_1]  expand build cmd error: {e}")
        return None

    pending_expansion_pts.add((cx, cy))
    print(f"[t_v6_1] FIRE  EXPAND CC @cluster ({cx},{cy}) "
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
        # v6: compact base. v5 used 20 tiles which let buildings
        # drift a screen away from the CC; 12 keeps them within a
        # short defense arc.
        "radius_tiles": 12,
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
        print(f"[t_v6_1]  find_placement error {unit_type_name(spec.type_id)}: {e}")
        return None
    spots = resp.get("spots", [])
    if not spots:
        return None
    # v6: skip spots that sit inside the mining corridor. `spot`
    # tiles are 32 px each; convert to the tile center in pixels.
    # The Refinery placement is exempt (it IS the geyser endpoint).
    neutrals = obs.get("neutrals", [])
    if spec.anchor != "geyser":
        keep = []
        for s in spots:
            sx = s["tile_x"] * 32 + 16
            sy = s["tile_y"] * 32 + 16
            if not in_mining_corridor(sx, sy, units, neutrals, main_type):
                keep.append(s)
        if not keep:
            print(f"[t_v6_1]  build {unit_type_name(spec.type_id)}: "
                  f"all {len(spots)} spots inside mining corridor; "
                  f"skipping this tick")
            return None
        spot = keep[0]
    else:
        spot = spots[0]

    completed, ip = count_units(units, spec.type_id)
    try:
        await c.build(unit_id=worker["unit_id"], unit_type=spec.type_id,
                      tile_x=spot["tile_x"], tile_y=spot["tile_y"])
    except Exception as e:
        print(f"[t_v6_1]  build cmd error {unit_type_name(spec.type_id)}: {e}")
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
        print(f"[t_v6_1]  train worker error: {e}")
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
        print(f"[t_v6_1]  train unit error: {e}")
        return None
    return Pending(verb="train", target_type=spec.type_id,
                   issued_frame=obs["current_frame"],
                   pre_min=r["minerals"], pre_gas=r["gas"],
                   pre_count=completed + ip,
                   cost_min=spec.cost_min, cost_gas=spec.cost_gas)


# --------------------------------------------------------------------
# v6 defense line: bunkers + tanks + turrets at a step-forward anchor.
# --------------------------------------------------------------------

# Step distances along the home -> enemy vector.
# v6_1: first bunker at 0.10 (very close to home -- SCV survives
# the walk with no combat units around yet). Only step out once
# the first bunker actually completes.
# 0.10 = first line, ~10 % of the way toward the enemy corner
# 0.20 = second (after first bunker up), 0.30 = third, ...
DEFENSE_STEP_START = 0.10
DEFENSE_STEP_DELTA = 0.10
DEFENSE_STEP_DELTA_PUSH = 0.15
DEFENSE_STEP_MAX = 0.85
# All tanks must sit within this many pixels of the current anchor
# before we allow stepping forward.
DEFENSE_LINE_TIGHT_PX = 400
# "Enemy nearby" radius used to VETO a step-forward in defensive
# mode (ignored in push mode).
DEFENSE_ENEMY_VETO_PX = 700
# Push-mode trigger: >= this many completed tanks near the current
# anchor and we start pushing the line forward.
PUSH_TANK_THRESHOLD = 3
# Marines patrol this far from the nearest bunker; keeps them within
# firing range while (until the server ships a `load` verb) actually
# entering the bunker isn't available.
MARINE_BUNKER_RING_PX = 200


def defense_anchor(home_x: int, home_y: int,
                   tgt_x: int, tgt_y: int,
                   step_frac: float,
                   map_w: int, map_h: int) -> tuple[int, int]:
    """Point along the home -> enemy vector at fraction step_frac.
    Falls back to a point step_frac toward map center when the
    enemy direction isn't known yet."""
    if tgt_x is None or tgt_y is None:
        tgt_x = map_w - home_x
        tgt_y = map_h - home_y
    ax = int(home_x + step_frac * (tgt_x - home_x))
    ay = int(home_y + step_frac * (tgt_y - home_y))
    return (ax, ay)


async def try_build_defense_structure(c: Client, obs: dict,
                                      structure_type: int,
                                      cost_min: int, cost_gas: int,
                                      anchor_x: int, anchor_y: int,
                                      worker_type: int, main_type: int,
                                      pending_workers: set[int]
                                      ) -> Pending | None:
    """Place a Bunker / Missile Turret exactly at the current defense
    anchor. Same corridor guard as try_build, but a smaller radius
    (5 tiles) so the structure lands near the anchor rather than
    drifting back to home."""
    r = obs["resources"]
    if r["minerals"] < cost_min or r["gas"] < cost_gas:
        return None
    units = obs["units"]
    cands = [u for u in workers(units)
             if u["type"] == worker_type
             and u["unit_id"] not in pending_workers]
    if not cands:
        return None
    # Pick the SCV nearest to the anchor, not the first mining one --
    # keeps the run short so mining loss is minimal.
    worker = min(cands, key=lambda u: dist_pixels(
        u["x"], u["y"], anchor_x, anchor_y))

    try:
        resp = await c.find_placement(
            unit_type=structure_type,
            worker_unit=worker["unit_id"],
            center_x=anchor_x, center_y=anchor_y,
            radius_tiles=5, max_results=8)
    except Exception as e:
        print(f"[t_v6_1]  defense find_placement error "
              f"{unit_type_name(structure_type)}: {e}")
        return None
    spots = resp.get("spots", [])
    if not spots:
        return None
    neutrals = obs.get("neutrals", [])
    keep = [s for s in spots
            if not in_mining_corridor(
                s["tile_x"] * 32 + 16, s["tile_y"] * 32 + 16,
                units, neutrals, main_type)]
    if not keep:
        return None
    spot = keep[0]

    completed, ip = count_units(units, structure_type)
    try:
        await c.build(unit_id=worker["unit_id"], unit_type=structure_type,
                      tile_x=spot["tile_x"], tile_y=spot["tile_y"])
    except Exception as e:
        print(f"[t_v6_1]  defense build cmd error "
              f"{unit_type_name(structure_type)}: {e}")
        return None
    return Pending(
        verb="build", target_type=structure_type,
        issued_frame=obs["current_frame"],
        pre_min=r["minerals"], pre_gas=r["gas"],
        pre_count=completed + ip,
        cost_min=cost_min, cost_gas=cost_gas,
        worker_id=worker["unit_id"],
    )


async def phase_defense_line(c: Client, obs: dict,
                             anchor_x: int, anchor_y: int,
                             home_x: int, home_y: int,
                             worker_type: int, main_type: int,
                             pending_workers: set[int],
                             pending: dict,
                             reserve,
                             bunker_target: int,
                             turret_target: int) -> None:
    """v6_1 defense pass (extends v6):

    (1) Ensure `bunker_target` bunkers exist near the current
        defense anchor (Terran_Bunker).
    (2) v6_1: ensure `turret_target` Missile Turrets total exist.
        Half the target near the defense anchor (anti-air /
        anti-cloak forward cover), half near home (anti-drop /
        anti-cloak base cover). Turrets rotate their placement
        point around the anchor / home to spread coverage.
    (3) Move any completed Siege Tank (both modes) that's more
        than DEFENSE_LINE_TIGHT_PX from the anchor toward the
        anchor. Sieging happens in Priority 7.7.
    (4) Marines patrol a ring near each bunker (load verb not
        available -- see SERVER_ASKS.md).
    """
    units = obs["units"]
    frame = obs["current_frame"]
    _BUNKER = UNIT_TYPES_BY_NAME["Terran_Bunker"]
    _TURRET = UNIT_TYPES_BY_NAME["Terran_Missile_Turret"]
    _MARINE = UNIT_TYPES_BY_NAME["Terran_Marine"]
    _TANK_MODE = UNIT_TYPES_BY_NAME["Terran_Siege_Tank_Tank_Mode"]
    _SIEGE_MODE = UNIT_TYPES_BY_NAME["Terran_Siege_Tank_Siege_Mode"]

    # (1) Bunkers.
    bunker_c, bunker_ip = count_units(units, _BUNKER)
    if (bunker_c + bunker_ip < bunker_target
            and f"build:{_BUNKER}" not in pending
            and reserve(100, 0)):
        p = await try_build_defense_structure(
            c, obs, _BUNKER, 100, 0,
            anchor_x, anchor_y,
            worker_type, main_type, pending_workers)
        if p is not None:
            pending[f"build:{_BUNKER}"] = p
            pending_workers.add(p.worker_id)
            print(f"[t_v6_1] FIRE  build:Bunker "
                  f"({bunker_c + bunker_ip + 1}/{bunker_target}) "
                  f"@anchor=({anchor_x},{anchor_y})")

    # (2) Missile Turrets. Needs Engineering Bay completed.
    # v6_1: rotate placement around both anchor and home so
    # turrets spread out for anti-air / anti-cloak coverage rather
    # than clustering on one tile. Half the target at each side,
    # anchor-side first (more urgent -- forward defense).
    _ENG_BAY = UNIT_TYPES_BY_NAME["Terran_Engineering_Bay"]
    eb_c, _ = count_units(units, _ENG_BAY)
    tur_c, tur_ip = count_units(units, _TURRET)
    if (eb_c > 0
            and tur_c + tur_ip < turret_target
            and f"build:{_TURRET}" not in pending
            and reserve(75, 0)):
        # Alternate between anchor and home; among the pair,
        # rotate through 4 angles so a second turret at the
        # same base doesn't stack on the first.
        idx = tur_c + tur_ip
        use_anchor = (idx < (turret_target + 1) // 2)
        base_x = anchor_x if use_anchor else home_x
        base_y = anchor_y if use_anchor else home_y
        # 4-way rotation, 160 px out. Jitter ~40 px so successive
        # runs don't land on the same pixel.
        angle_deg = (idx * 90) % 360
        angle = angle_deg * math.pi / 180.0
        spread = 160
        tx = int(base_x + math.cos(angle) * spread
                 + random.randint(-40, 40))
        ty = int(base_y + math.sin(angle) * spread
                 + random.randint(-40, 40))
        p = await try_build_defense_structure(
            c, obs, _TURRET, 75, 0,
            tx, ty,
            worker_type, main_type, pending_workers)
        if p is not None:
            pending[f"build:{_TURRET}"] = p
            pending_workers.add(p.worker_id)
            print(f"[t_v6_1] FIRE  build:Missile_Turret "
                  f"({tur_c + tur_ip + 1}/{turret_target}) "
                  f"@{'anchor' if use_anchor else 'home'}=({tx},{ty})")

    # (3) Move stray tanks to the anchor. Only touch tanks that
    # aren't already on the way (order == Move) or fighting
    # (AttackUnit). Sieged tanks stay put -- unsiege is handled
    # by the existing siege-hysteresis pass, and we don't want to
    # spam-toggle here.
    for tank in units:
        if not tank.get("completed"):
            continue
        if tank["type"] not in (_TANK_MODE,):
            continue  # sieged tanks don't move
        d = dist_pixels(tank["x"], tank["y"], anchor_x, anchor_y)
        if d < DEFENSE_LINE_TIGHT_PX:
            continue
        order_str = order_name(tank["order"])
        if order_str in ("Move", "AttackMove", "AttackUnit"):
            continue
        try:
            await c.move(unit_id=tank["unit_id"],
                         x=anchor_x, y=anchor_y)
            print(f"[t_v6_1]  DEFENSE move tank {tank['unit_id']} "
                  f"-> anchor ({anchor_x},{anchor_y}) "
                  f"was {d:.0f}px away")
        except Exception as e:
            print(f"[t_v6_1]  defense-tank move error: {e}")

    # (4) Marines: patrol a ring near the nearest bunker.
    bunkers = [u for u in units if u["type"] == _BUNKER
               and u.get("completed") is True]
    if bunkers:
        for m in units:
            if m.get("type") != _MARINE or not m.get("completed"):
                continue
            if m["order"] not in IDLE_ORDERS:
                # only re-order when the marine is truly idle -- don't
                # yank one out of an attack
                continue
            b = min(bunkers, key=lambda u: dist_pixels(
                u["x"], u["y"], m["x"], m["y"]))
            if dist_pixels(m["x"], m["y"], b["x"], b["y"]) < MARINE_BUNKER_RING_PX:
                continue
            # Random point on the bunker's ring, biased toward the
            # enemy vector (frame-based seed keeps determinism).
            angle = ((frame + m["unit_id"] * 37) % 360) * math.pi / 180.0
            mx = int(b["x"] + math.cos(angle) * MARINE_BUNKER_RING_PX)
            my = int(b["y"] + math.sin(angle) * MARINE_BUNKER_RING_PX)
            try:
                await c.move(unit_id=m["unit_id"], x=mx, y=my)
            except Exception as e:
                print(f"[t_v6_1]  marine-ring move error: {e}")


def defense_line_ready(units: list[dict],
                       anchor_x: int, anchor_y: int) -> bool:
    """True when it's safe to step the defense anchor forward:
    all completed Siege Tanks (either mode) sit within
    DEFENSE_LINE_TIGHT_PX of the current anchor, AND we have at
    least one such tank."""
    _TANK_MODE = UNIT_TYPES_BY_NAME["Terran_Siege_Tank_Tank_Mode"]
    _SIEGE_MODE = UNIT_TYPES_BY_NAME["Terran_Siege_Tank_Siege_Mode"]
    tanks = [u for u in units
             if u["type"] in (_TANK_MODE, _SIEGE_MODE)
             and u.get("completed")]
    if not tanks:
        return False
    for t in tanks:
        if dist_pixels(t["x"], t["y"], anchor_x, anchor_y) > DEFENSE_LINE_TIGHT_PX:
            return False
    return True


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
        print(f"[t_v6_1] FIRE  {spec.kind}:{spec.label} @{src['unit_id']} "
              f"cost={spec.cost_min}/{spec.cost_gas}")
        return True
    except Exception as e:
        print(f"[t_v6_1]  {spec.kind} error: {e}")
        return False


# --------------------------------------------------------------------
# Main loop.
# --------------------------------------------------------------------

async def run(c: Client, interval_sec: float,
              worker_target: int, supply_slack: int,
              worker_train_min: int, pylon_target: int,
              scout_radial: int, scout_zscan: int,
              base_target: int,
              bunker_target: int,
              turret_target: int,
              defense_only: bool,
              push_after_frames: int) -> None:
    print(f"[t_v6_1] connected slot={c.welcome.slot} "
          f"frame={c.welcome.current_frame}")
    print(f"[t_v6_1] turtle config: "
          f"bunker_target={bunker_target} "
          f"turret_target={turret_target} "
          f"defense_only={defense_only} "
          f"base_target={base_target} "
          f"push_after_frames={push_after_frames}")

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

    # v6 defense-line state.
    # `defense_step`: current fraction along the home -> enemy vector
    # where we're building the defensive wall. Starts at
    # DEFENSE_STEP_START and grows by DEFENSE_STEP_DELTA every time
    # `defense_line_ready` returns True with no enemy nearby.
    defense_step: float = DEFENSE_STEP_START
    last_step_advance_frame: int = -1

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
            print(f"[t_v6_1] race={race} home=({home_x},{home_y}) "
                  f"map={map_w}x{map_h}")
            print(f"[t_v6_1] radial wps: {waypoints_by_mode['radial']}")
            print(f"[t_v6_1] zscan wps: {len(waypoints_by_mode['zscan'])} points "
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
                    print(f"[t_v6_1] TOOK  upgrade:{spec.label} "
                          f"(level {lvl})")
            else:  # research
                if spec.enum_id in obs_tech:
                    completed_upgrades.add(key)
                    print(f"[t_v6_1] TOOK  research:{spec.label}")

        # Compute attack target: nearest known enemy building; else fall
        # back to opposite corner (v2 behavior).
        if known_enemies:
            e = min(known_enemies.values(),
                    key=lambda k: dist_pixels(home_x, home_y, k.x, k.y))
            tgt_x, tgt_y = e.x, e.y
        else:
            tgt_x = map_w - home_x
            tgt_y = map_h - home_y

        # v6_1: defense anchor + step-forward.
        # Anchor = point at `defense_step` along home -> tgt.
        # Advance rules:
        #   DEFENSIVE mode (default):
        #     - line tight (all tanks within DEFENSE_LINE_TIGHT_PX)
        #     - no enemy within DEFENSE_ENEMY_VETO_PX of anchor
        #     - >= 500 frames since last advance
        #     - step += DEFENSE_STEP_DELTA (0.10)
        #   PUSH mode:
        #     - triggered by (a) >= PUSH_TANK_THRESHOLD tanks at
        #       the anchor OR (b) frame >= push_after_frames
        #     - line tight still required, but enemy-near-anchor
        #       veto is dropped (we intentionally step INTO enemy
        #       vision to keep taking ground)
        #     - >= 300 frames since last advance (faster cadence)
        #     - step += DEFENSE_STEP_DELTA_PUSH (0.15)
        #     - off-anchor sieged tanks unsiege so they can walk
        #       to the new anchor
        anchor_x, anchor_y = defense_anchor(
            home_x, home_y, tgt_x, tgt_y,
            defense_step, map_w, map_h)

        _TANK_MODE_ID = UNIT_TYPES_BY_NAME["Terran_Siege_Tank_Tank_Mode"]
        _SIEGE_MODE_ID = UNIT_TYPES_BY_NAME["Terran_Siege_Tank_Siege_Mode"]
        tanks_at_anchor = sum(
            1 for u in units
            if u["type"] in (_TANK_MODE_ID, _SIEGE_MODE_ID)
            and u.get("completed")
            and dist_pixels(u["x"], u["y"],
                            anchor_x, anchor_y) < DEFENSE_LINE_TIGHT_PX)
        push_mode = (tanks_at_anchor >= PUSH_TANK_THRESHOLD
                     or frame >= push_after_frames)

        enemy_near_anchor = False
        for e in obs.get("enemies", []):
            if dist_pixels(e["x"], e["y"],
                           anchor_x, anchor_y) <= DEFENSE_ENEMY_VETO_PX:
                enemy_near_anchor = True
                break

        step_delta = (DEFENSE_STEP_DELTA_PUSH if push_mode
                      else DEFENSE_STEP_DELTA)
        rate_limit_f = 300 if push_mode else 500
        # v6_1: hold at the initial 0.10 anchor until at least one
        # Bunker is *completed*. Otherwise the SCV keeps getting
        # picked off on the walk to a more forward anchor.
        _BUNKER_TYPE_STEP = UNIT_TYPES_BY_NAME["Terran_Bunker"]
        bunker_done_now, _ = count_units(units, _BUNKER_TYPE_STEP)
        allow_advance = (
            defense_step < DEFENSE_STEP_MAX
            and bunker_done_now >= 1
            and defense_line_ready(units, anchor_x, anchor_y)
            and (push_mode or not enemy_near_anchor)
            and frame - last_step_advance_frame > rate_limit_f
        )
        if allow_advance:
            defense_step = min(DEFENSE_STEP_MAX,
                               defense_step + step_delta)
            last_step_advance_frame = frame
            new_anchor = defense_anchor(
                home_x, home_y, tgt_x, tgt_y,
                defense_step, map_w, map_h)
            mode_str = "PUSH" if push_mode else "def"
            print(f"[t_v6_1]  DEFENSE step advanced to {defense_step:.2f} "
                  f"[{mode_str}] @frame {frame}; new anchor={new_anchor} "
                  f"(tanks_at_anchor={tanks_at_anchor})")
            # In push mode, unsiege every sieged tank that's now
            # far from the NEW anchor so they can walk forward.
            if push_mode:
                nax, nay = new_anchor
                for tank in units:
                    if tank["type"] != _SIEGE_MODE_ID:
                        continue
                    if not tank.get("completed"):
                        continue
                    if dist_pixels(tank["x"], tank["y"],
                                   nax, nay) < DEFENSE_LINE_TIGHT_PX:
                        continue
                    try:
                        await c.unsiege(unit_id=tank["unit_id"])
                        print(f"[t_v6_1]  PUSH unsiege tank "
                              f"{tank['unit_id']} to catch up to new anchor")
                    except Exception as e:
                        print(f"[t_v6_1]  push-unsiege error: {e}")

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
        print(f"[t_v6_1] f={frame} min={r['minerals']} gas={r['gas']} "
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

        # ---- Priority 0.5: scouting (does NOT reserve budget).
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
                            radius_tiles=10, max_results=8)
                        spots = resp.get("spots", [])
                        # v6: corridor guard for depot too.
                        neutrals_now = obs.get("neutrals", [])
                        spots = [s for s in spots
                                 if not in_mining_corridor(
                                     s["tile_x"] * 32 + 16,
                                     s["tile_y"] * 32 + 16,
                                     units, neutrals_now, main_type)]
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
                            print(f"[t_v6_1] FIRE  build:Supply_Depot "
                                  f"({pyl_total2 + 1}/{pylon_target}) "
                                  f"anchor={anchor_pt}")
                        else:
                            budget["min"] += 100  # refund
                    except Exception as e:
                        print(f"[t_v6_1]  supply-depot fire error: {e}")
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
                    print(f"[t_v6_1] FIRE  train:SCV ({n_workers + 1}/{worker_target})")
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
                    print(f"[t_v6_1] FIRE  build:Refinery")
                else:
                    budget["min"] += 100

        # ---- v6 Priority 4.25: defense line.
        # Bunkers + Missile Turrets + tank-to-anchor. Runs BEFORE
        # expansion and catalog buildings so it gets first crack at
        # the mineral budget -- defensive structures are cheaper
        # than a CC and much more valuable early game.
        await phase_defense_line(
            c, obs, anchor_x, anchor_y, home_x, home_y,
            worker_type, main_type, pending_workers, pending,
            reserve, bunker_target, turret_target)

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

        # v6_1 gating: expand as soon as one Bunker is up. The
        # tank requirement in v6 was self-defeating -- tanks need
        # gas, gas needs a second geyser, second geyser needs an
        # expansion. One completed Bunker is enough to protect the
        # 400-min investment while the CC comes online. The
        # "our-side" cluster preference (pick_expansion_site) still
        # keeps the second base on our half of the map.
        _BUNKER_TYPE = UNIT_TYPES_BY_NAME["Terran_Bunker"]
        bunker_ready, _ = count_units(units, _BUNKER_TYPE)
        turtle_ready = (bunker_ready >= 1)

        if (nx_c + nx_ip < base_target and nx_key not in pending
                and len(known_resources) >= 4
                and turtle_ready):
            if reserve(400, 0):
                p = await try_expand(
                    c, obs, main_type, worker_type,
                    known_resources, pending_expansion_pts,
                    pending_workers,
                    known_enemies=known_enemies,
                    home_x=home_x, home_y=home_y)
                if p is not None:
                    pending[nx_key] = p
                    pending_workers.add(p.worker_id)
                else:
                    budget["min"] += 400  # refund
        elif nx_c + nx_ip < base_target and not turtle_ready:
            if frame % 200 == 0:
                print(f"[t_v6_1]  EXPAND holdoff: "
                      f"bunker_ready={bunker_ready}/1")

        # ---- Priority 5: catalog buildings (1 per tick).
        # v6_1: respect spec.target_count (Barracks/Factory=2 for
        # redundancy under attack; everything else defaults to 1).
        catalog_build_this_tick = 0
        for spec in catalog_buildings:
            key = f"build:{spec.type_id}"
            if key in pending: continue
            completed, ip = count_units(units, spec.type_id)
            if completed + ip >= spec.target_count: continue
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
                print(f"[t_v6_1] FIRE  build:{unit_type_name(spec.type_id)}")
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
                print(f"[t_v6_1] FIRE  addon:{aname} on "
                      f"{unit_type_name(parent_type)} {parent['unit_id']}")
                addon_fired = True
            except Exception as e:
                budget["min"] += amin; budget["gas"] += agas
                print(f"[t_v6_1]  addon fire error: {e}")

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
                print(f"[t_v6_1] FIRE  train:{unit_type_name(spec.type_id)} "
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

        # ---- Priority 7.4: lift-to-safety (v5 pass E).
        # For each own lift-capable building at critical HP (< 30 %)
        # WITH a visible enemy in range, fire `lift` and float back
        # to home. Repair pass (7.5) can still heal it airborne --
        # SCVs can repair flying buildings.
        # Skips re-landing: this v5 doesn't auto-land the building;
        # a follow-up (v5.1) can add "re-land at safe tile when HP
        # > 80 %". For now the lifted building just floats near home.
        # Rate-limit: 1 lift per tick.
        _LIFT_TYPES = {
            UNIT_TYPES_BY_NAME["Terran_Command_Center"],
            UNIT_TYPES_BY_NAME["Terran_Barracks"],
            UNIT_TYPES_BY_NAME["Terran_Factory"],
            UNIT_TYPES_BY_NAME["Terran_Starport"],
            UNIT_TYPES_BY_NAME["Terran_Science_Facility"],
        }
        LIFT_ENEMY_RANGE_PX = 600
        lifted = False
        for bld in units:
            if lifted: break
            if bld.get("type") not in _LIFT_TYPES: continue
            if not bld.get("completed"): continue
            if bld.get("flying"): continue          # already airborne
            hp = bld.get("hp"); hp_max = bld.get("hp_max")
            if not hp or not hp_max: continue
            if hp * 10 >= hp_max * 3: continue      # HP >= 30 %, safe
            # Only lift if there's an enemy nearby -- lifting a
            # building the sim never intended to be damaged just
            # freezes production for no reason.
            near = False
            for e in enemies_visible:
                if dist_pixels(e["x"], e["y"],
                               bld["x"], bld["y"]) <= LIFT_ENEMY_RANGE_PX:
                    near = True
                    break
            if not near: continue
            try:
                await c.lift(unit_id=bld["unit_id"], x=home_x, y=home_y)
                print(f"[t_v6_1] FIRE  lift "
                      f"{unit_type_name(bld['type'])} {bld['unit_id']} "
                      f"hp={hp}/{hp_max} -> retreat to ({home_x},{home_y})")
                lifted = True
            except Exception as e:
                print(f"[t_v6_1]  lift error: {e}")

        # ---- Priority 7.5: SCV repair on damaged mechanical assets.
        # Terran's answer to Protoss v4's fighter refill: instead of
        # topping up interceptor/scarab counts we keep our late-game
        # mech units and buildings alive. For each damaged mechanical
        # target, pull the nearest idle SCV and dispatch a repair.
        # Rate-limited to 3 fires per tick so a hail-of-damage moment
        # doesn't pull the entire mining fleet off minerals.
        #
        # Doesn't reserve budget: minerals + gas are consumed over
        # time by the repair process, not up-front. The sim silent-
        # rejects if the SCV can't reach the target or the target is
        # bio/undamaged/dead by the time the action lands.
        repair_fires = 0
        # Skip SCVs already repairing (their order == Repair) or
        # already engaged in this tick's build/scout/gas pipelines.
        repair_ord = ORDERS_BY_NAME.get("Repair", 34)
        busy_scvs = set(pending_workers)
        for pu in units:
            if pu.get("type") == _SCV and pu.get("order") == repair_ord:
                busy_scvs.add(pu["unit_id"])

        # Rebuild the damaged list explicitly (the status-line loop
        # only counted them; we need the entries themselves).
        damaged = [u for u in units if is_repairable(u)]
        # Prefer high-value targets first: buildings first, then
        # tanks / battlecruisers, then everything else. Cheap heuristic
        # sort by hp_missing descending as a tie-breaker.
        _HIGH = {
            UNIT_TYPES_BY_NAME["Terran_Siege_Tank_Tank_Mode"],
            UNIT_TYPES_BY_NAME["Terran_Siege_Tank_Siege_Mode"],
            UNIT_TYPES_BY_NAME["Terran_Battlecruiser"],
            UNIT_TYPES_BY_NAME["Terran_Science_Vessel"],
        }
        def _repair_priority(u):
            is_building = bool(u.get("building"))
            hp_missing = int(u.get("hp_max", 0)) - int(u.get("hp", 0))
            tier = 0 if is_building else (1 if u["type"] in _HIGH else 2)
            return (tier, -hp_missing)
        damaged.sort(key=_repair_priority)

        # Pool of idle SCVs: not building, not scouting, not gathering
        # gas (Harvest orders 78-89 range roughly), just standing or
        # mining minerals. Mining SCVs are fair game -- pulling one for
        # a repair is cheap short-term.
        _MINING_ORDERS = {
            ORDERS_BY_NAME.get("MoveToMinerals", 80),
            ORDERS_BY_NAME.get("WaitForMinerals", 81),
            ORDERS_BY_NAME.get("MiningMinerals", 82),
            ORDERS_BY_NAME.get("ReturnMinerals", 84),
            ORDERS_BY_NAME.get("Harvest1", 78),
            ORDERS_BY_NAME.get("Harvest2", 79),
        }
        _MINING_ORDERS.discard(None)
        idle_scvs = [u for u in units
                     if u.get("type") == _SCV
                     and u.get("completed")
                     and u["unit_id"] not in busy_scvs
                     and (u.get("order") in IDLE_ORDERS
                          or u.get("order") in _MINING_ORDERS)]

        for tgt in damaged:
            if repair_fires >= 3:
                break
            if not idle_scvs:
                break
            # Pick nearest idle SCV to target.
            scv = min(idle_scvs, key=lambda s: dist_pixels(
                s["x"], s["y"], tgt["x"], tgt["y"]))
            try:
                await c.repair(unit_id=scv["unit_id"],
                               target_unit=tgt["unit_id"])
                print(f"[t_v6_1] FIRE  repair "
                      f"scv={scv['unit_id']} -> "
                      f"{unit_type_name(tgt['type'])} {tgt['unit_id']} "
                      f"hp={tgt.get('hp')}/{tgt.get('hp_max')}")
                repair_fires += 1
                idle_scvs.remove(scv)
                busy_scvs.add(scv["unit_id"])
            except Exception as e:
                print(f"[t_v6_1]  repair error: {e}")

        # ---- Priority 7.7: auto-siege / auto-unsiege (v5 pass C).
        # Tanks in Tank_Mode with a live enemy in range fire `siege`.
        # Tanks in Siege_Mode with NO enemy in a wider range fire
        # `unsiege` (hysteresis avoids spam-toggle when a marine
        # ping-pongs across the siege/unsiege boundary).
        # Prereq: Tank_Siege_Mode tech researched (TechTypes id 5,
        # in obs_tech set). Sim silent-rejects without the tech, but
        # we skip fires anyway to keep the log clean.
        _TANK_MODE = UNIT_TYPES_BY_NAME["Terran_Siege_Tank_Tank_Mode"]
        _SIEGE_MODE = UNIT_TYPES_BY_NAME["Terran_Siege_Tank_Siege_Mode"]
        _TECH_SIEGE_MODE = 5
        SIEGE_RANGE_PX = 320   # ~10 tiles -- Siege Mode attack range
        UNSIEGE_RANGE_PX = 448  # 14 tiles -- must be OUT this far to lift
        siege_fires = 0
        if _TECH_SIEGE_MODE in obs_tech:
            for tank in units:
                if siege_fires >= 3: break
                if not tank.get("completed"): continue
                t_type = tank["type"]
                if t_type not in (_TANK_MODE, _SIEGE_MODE): continue
                # Nearest visible enemy distance.
                if enemies_visible:
                    ne = min(enemies_visible,
                             key=lambda e: dist_pixels(
                                 e["x"], e["y"], tank["x"], tank["y"]))
                    ne_dist = dist_pixels(
                        ne["x"], ne["y"], tank["x"], tank["y"])
                else:
                    ne_dist = 1_000_000
                # v6: tanks sitting at the defense anchor siege
                # unconditionally, even without an enemy in view --
                # we want the line ARMED before the fight starts.
                at_anchor = dist_pixels(
                    tank["x"], tank["y"],
                    anchor_x, anchor_y) < DEFENSE_LINE_TIGHT_PX
                should_siege = (
                    t_type == _TANK_MODE
                    and (ne_dist <= SIEGE_RANGE_PX or at_anchor))
                if should_siege:
                    try:
                        await c.siege(unit_id=tank["unit_id"])
                        reason = (f"enemy@{ne_dist}px"
                                  if ne_dist <= SIEGE_RANGE_PX
                                  else "at-anchor")
                        print(f"[t_v6_1] FIRE  siege tank={tank['unit_id']} "
                              f"({reason})")
                        siege_fires += 1
                    except Exception as e:
                        print(f"[t_v6_1]  siege error: {e}")
                elif (t_type == _SIEGE_MODE
                        and ne_dist >= UNSIEGE_RANGE_PX
                        and not at_anchor):
                    # Only unsiege if we're OFF the anchor and no
                    # enemy in range -- otherwise the anchor tanks
                    # ping-pong between modes.
                    try:
                        await c.unsiege(unit_id=tank["unit_id"])
                        print(f"[t_v6_1] FIRE  unsiege tank={tank['unit_id']} "
                              f"(no enemy within {UNSIEGE_RANGE_PX}px, "
                              f"off anchor)")
                        siege_fires += 1
                    except Exception as e:
                        print(f"[t_v6_1]  unsiege error: {e}")

        # ---- Priority 7.8: Vulture Spider Mine drop (v5 pass D).
        # For each Vulture with mines remaining and Spider_Mines
        # tech researched, drop a mine at a point along the home->
        # enemy vector. Progress each vulture's mine placements
        # further along the vector (30%, 50%, 70%) so we don't stack
        # all mines on top of each other. `mine_count_by_vulture`
        # tracks per-unit remaining mines (each Vulture ships with 3).
        # Sim silent-rejects if the Vulture actually has 0 mines
        # remaining, but our client-side counter usually agrees.
        _VULTURE = UNIT_TYPES_BY_NAME["Terran_Vulture"]
        _TECH_SPIDER_MINES = 3
        mine_fires = 0
        if _TECH_SPIDER_MINES in obs_tech and tgt_x is not None:
            for vult in units:
                if mine_fires >= 2: break
                if vult.get("type") != _VULTURE: continue
                if not vult.get("completed"): continue
                vid = vult["unit_id"]
                remaining = mine_count_by_vulture.get(vid, 3)
                if remaining <= 0: continue
                # Progress along home -> tgt vector. First drop at
                # 30 % of the way, second at 50 %, third at 70 %.
                drop_idx = 3 - remaining   # 0, 1, 2
                t = 0.30 + drop_idx * 0.20
                mx = int(home_x + t * (tgt_x - home_x))
                my = int(home_y + t * (tgt_y - home_y))
                try:
                    await c.place_mine(unit_id=vid, x=mx, y=my)
                    mine_count_by_vulture[vid] = remaining - 1
                    print(f"[t_v6_1] FIRE  place_mine vulture={vid} "
                          f"@({mx},{my}) drop {drop_idx+1}/3")
                    mine_fires += 1
                except Exception as e:
                    print(f"[t_v6_1]  place_mine error: {e}")

        # ---- Priority 8: attack (idle combat -> nearest known enemy).
        # v6: when `defense_only` is set (default), idle combat units
        # stay on the defense line -- we still respond to enemies that
        # walk into vision, but no one wanders off toward the enemy
        # base. When `defense_only=False`, behaviour matches v5.
        for u in combat_units(units):
            if u["order"] not in IDLE_ORDERS: continue
            try:
                enemies = obs.get("enemies", [])
                # Prefer enemy in current vision (nearest to unit).
                if enemies:
                    t = nearest(u, enemies)
                    if t is not None:
                        await c.attack(unit_id=u["unit_id"],
                                       target_unit=t["unit_id"])
                        continue
                if defense_only:
                    # No visible enemy AND defense-only mode: hold
                    # ground. The defense-line pass already moved
                    # tanks / marines into position; nothing to do
                    # here except leave them idle.
                    continue
                # Non-defense-only fallback (v5 behaviour): attack-
                # move toward known enemy base (or fallback corner).
                if tgt_x is not None:
                    await c.attack(unit_id=u["unit_id"], target_unit=0,
                                   x=tgt_x, y=tgt_y)
            except Exception as e:
                print(f"[t_v6_1]  attack error: {e}")

        # ---- Priority 9: coverage verbs.
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
                    print(f"[t_v6_1]  cover move error: {e}")
        if move_done and not stop_done:
            cands = [u for u in units if u["order"] not in IDLE_ORDERS
                     and not u.get("building")]
            if cands:
                try:
                    await c.stop(unit_id=cands[0]["unit_id"])
                    stop_done = True
                except Exception as e:
                    print(f"[t_v6_1]  cover stop error: {e}")

        await asyncio.sleep(interval_sec)


async def main(api_key, host, port, url, interval_sec, worker_target,
               supply_slack, worker_train_min, pylon_target,
               scout_radial, scout_zscan, base_target,
               bunker_target, turret_target, defense_only,
               push_after_frames):
    if url:
        client_kwargs = {"api_key": api_key, "url": url}
    else:
        client_kwargs = {"api_key": api_key, "host": host, "port": port}
    async with Client(**client_kwargs) as c:
        await run(c, interval_sec, worker_target, supply_slack,
                  worker_train_min, pylon_target,
                  scout_radial, scout_zscan, base_target,
                  bunker_target, turret_target, defense_only,
                  push_after_frames)


def entrypoint() -> None:
    p = argparse.ArgumentParser(
        prog="python3 -m python_agent.agents.t_agent_v6_1",
        description="Terran v6_1: v6 turtle + earlier expansion + "
                    "more turrets + push-forward mode.")
    p.add_argument("api_key")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=6113)
    p.add_argument("--url", default=None,
                   help="full wss://.../agent URL (overrides --host/--port); "
                        "use this to connect through the simsc ALB")
    p.add_argument("--interval-sec", type=float, default=1.5)
    p.add_argument("--worker-target", type=int, default=40)
    p.add_argument("--supply-slack", type=int, default=8)
    p.add_argument("--worker-train-min", type=int, default=50)
    p.add_argument("--pylon-target", type=int, default=20)
    p.add_argument("--scout-radial", type=int, default=1,
                   help="probes doing 8-point radial ring patrol")
    p.add_argument("--scout-zscan", type=int, default=1,
                   help="probes doing Z-shape sweep of the whole map")
    p.add_argument("--base-target", type=int, default=4,
                   help="target total CC count including main base")
    p.add_argument("--bunker-target", type=int, default=3,
                   help="target Bunker count along the defense line")
    p.add_argument("--turret-target", type=int, default=4,
                   help="target Missile Turret count (spread between "
                        "the defense anchor and home)")
    p.add_argument("--defense-only", type=int, default=1,
                   help="1 (default) = pure turtle: idle combat units "
                        "hold the defense line. 0 = also send idle "
                        "combat toward known enemy base (v5 behaviour).")
    p.add_argument("--push-after-frames", type=int, default=25000,
                   help="Frame at which push-forward mode auto-arms "
                        "even without >= PUSH_TANK_THRESHOLD tanks. "
                        "Default 25000 (~4 min at game-speed 10).")
    args = p.parse_args()
    try:
        asyncio.run(main(args.api_key, args.host, args.port, args.url,
                         args.interval_sec, args.worker_target,
                         args.supply_slack, args.worker_train_min,
                         args.pylon_target,
                         args.scout_radial, args.scout_zscan,
                         args.base_target,
                         args.bunker_target,
                         args.turret_target,
                         bool(args.defense_only),
                         args.push_after_frames))
    except KeyboardInterrupt:
        print("\n[t_v6_1] stopped")


if __name__ == "__main__":
    entrypoint()
