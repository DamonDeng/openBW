"""p_agent_v4: v3 + carrier/reaver fighter maintenance.

Adds one new capability on top of v3's already-comprehensive agent:

  * Fighter refill: for every own Protoss Carrier and Reaver, if the
    unit's fighter count + queued < fighter max AND we can afford
    another fighter, fire the new `train_fighter` verb. Interceptors
    (Carrier's babies) cost 25 min each, Scarabs (Reaver's babies)
    cost 15 min. Both cap at 8 (Carrier w/ Capacity upgrade) / 5
    (Reaver w/o Reaver_Capacity) respectively.

    Uses the observation's new `fighter_count` / `fighter_queued` /
    `fighter_max` fields (added on the server side alongside the
    train_fighter verb) so we fire only when a specific parent needs
    a refill -- no wasted commands.

Everything else is inherited from v3: scouting (radial + zscan),
wider building spread (toward map center), upgrades/tech, expansions
(up to 4 bases), verbose scout logs.

Zerg still out of scope. Terran also has no fighter mechanics in
this catalog -- train_fighter only applies to Protoss Carrier/Reaver.

Usage:
    python3 -m python_agent.agents.p_agent_v4 <api_key>
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
# Race catalogs (Protoss primary; Terran mostly reused).
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
    supply_each: int = 0
    # How many of this unit type to maintain (completed + in-progress).
    # Cheap ground units get big targets so an army actually forms;
    # expensive/late-tier units (Carrier, Arbiter) stay low so we still
    # try one or two for coverage without dumping all resources on
    # them. Default 1 keeps original v2/v3 "one of each" semantics
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
    # Cheap Gateway units -- large army targets.
    UnitSpec(UNIT_TYPES_BY_NAME["Protoss_Zealot"],
             UNIT_TYPES_BY_NAME["Protoss_Gateway"],       100,   0, 2, target_count=8),
    UnitSpec(UNIT_TYPES_BY_NAME["Protoss_Dragoon"],
             UNIT_TYPES_BY_NAME["Protoss_Gateway"],       125,  50, 2, target_count=8),
    UnitSpec(UNIT_TYPES_BY_NAME["Protoss_High_Templar"],
             UNIT_TYPES_BY_NAME["Protoss_Gateway"],        50, 150, 2, target_count=2),
    UnitSpec(UNIT_TYPES_BY_NAME["Protoss_Dark_Templar"],
             UNIT_TYPES_BY_NAME["Protoss_Gateway"],       125, 100, 2, target_count=2),
    # Robotics -- medium cost, medium count.
    UnitSpec(UNIT_TYPES_BY_NAME["Protoss_Reaver"],
             UNIT_TYPES_BY_NAME["Protoss_Robotics_Facility"], 200, 100, 4, target_count=2),
    UnitSpec(UNIT_TYPES_BY_NAME["Protoss_Observer"],
             UNIT_TYPES_BY_NAME["Protoss_Robotics_Facility"],  25,  75, 1, target_count=2),
    UnitSpec(UNIT_TYPES_BY_NAME["Protoss_Shuttle"],
             UNIT_TYPES_BY_NAME["Protoss_Robotics_Facility"], 200,   0, 2, target_count=1),
    # Stargate -- expensive air, low counts.
    UnitSpec(UNIT_TYPES_BY_NAME["Protoss_Scout"],
             UNIT_TYPES_BY_NAME["Protoss_Stargate"],      275, 125, 3, target_count=2),
    UnitSpec(UNIT_TYPES_BY_NAME["Protoss_Corsair"],
             UNIT_TYPES_BY_NAME["Protoss_Stargate"],      150, 100, 2, target_count=2),
    UnitSpec(UNIT_TYPES_BY_NAME["Protoss_Carrier"],
             UNIT_TYPES_BY_NAME["Protoss_Stargate"],      350, 250, 6, target_count=2),
    UnitSpec(UNIT_TYPES_BY_NAME["Protoss_Arbiter"],
             UNIT_TYPES_BY_NAME["Protoss_Stargate"],      100, 350, 4, target_count=1),
]

# Protoss upgrade catalog. Enum values from bwenums.h:
#   UpgradeTypes: Protoss_Ground_Armor=5, Protoss_Air_Armor=6,
#                 Protoss_Ground_Weapons=13, Protoss_Air_Weapons=14,
#                 Protoss_Plasma_Shields=15, Singularity_Charge=33,
#                 Leg_Enhancements=34, Scarab_Damage=35, Reaver_Capacity=36,
#                 Gravitic_Drive=37, ...
#   TechTypes:    Psionic_Storm=19, Hallucination=20, Recall=21, Stasis_Field=22
PROTOSS_UPGRADES: list[UpgradeSpec] = [
    # Forge upgrades -- available immediately after Forge completes.
    UpgradeSpec("upgrade", 13, UNIT_TYPES_BY_NAME["Protoss_Forge"],
                100, 100, "GroundWeapons_L1"),
    UpgradeSpec("upgrade", 5, UNIT_TYPES_BY_NAME["Protoss_Forge"],
                100, 100, "GroundArmor_L1"),
    UpgradeSpec("upgrade", 15, UNIT_TYPES_BY_NAME["Protoss_Forge"],
                200, 200, "PlasmaShields_L1"),
    # Cybernetics Core upgrades.
    UpgradeSpec("upgrade", 14, UNIT_TYPES_BY_NAME["Protoss_Cybernetics_Core"],
                100, 100, "AirWeapons_L1"),
    UpgradeSpec("upgrade", 6, UNIT_TYPES_BY_NAME["Protoss_Cybernetics_Core"],
                150, 150, "AirArmor_L1"),
    UpgradeSpec("upgrade", 33, UNIT_TYPES_BY_NAME["Protoss_Cybernetics_Core"],
                150, 150, "SingularityCharge"),
    # Citadel of Adun -- Zealot speed.
    UpgradeSpec("upgrade", 34, UNIT_TYPES_BY_NAME["Protoss_Citadel_of_Adun"],
                150, 150, "LegEnhancements"),
    # Templar Archives techs.
    UpgradeSpec("research", 19, UNIT_TYPES_BY_NAME["Protoss_Templar_Archives"],
                200, 200, "PsionicStorm"),
    UpgradeSpec("research", 20, UNIT_TYPES_BY_NAME["Protoss_Templar_Archives"],
                150, 150, "Hallucination"),
    # Robotics Support Bay -- Reaver.
    UpgradeSpec("upgrade", 35, UNIT_TYPES_BY_NAME["Protoss_Robotics_Support_Bay"],
                200, 200, "ScarabDamage"),
]


def race_catalogs(race: str):
    if race == "protoss":
        return (PROTOSS_BUILDINGS, PROTOSS_UNITS, PROTOSS_UPGRADES,
                UNIT_TYPES_BY_NAME["Protoss_Probe"],
                UNIT_TYPES_BY_NAME["Protoss_Pylon"],
                UNIT_TYPES_BY_NAME["Protoss_Nexus"])
    raise SystemExit(f"[p_v4] race={race} not supported yet")


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
            print(f"[p_v4] TOOK  {p.label():48s} "
                  f"(count {p.pre_count}->{cur_count} after {age}f)")
            to_drop.append(key)
            continue
        if age >= grace_frames:
            stats.reject[p.label()] += 1
            n = stats.reject[p.label()]
            if n == 1 or n % 5 == 0:
                print(f"[p_v4] REJECT {p.label():48s} after {age}f. n={n}. "
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
                    print(f"[p_v4]  gather-gas error: {e}")

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
                print(f"[p_v4]  gather-min error: {e}")

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
            print(f"[p_v4]  SCOUT worker {wid} died; unassigning")
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
            print(f"[p_v4]  SCOUT worker {w['unit_id']} mode={mode} "
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
            print(f"[p_v4]  SCOUT {sc.worker_id} [{sc.mode}] all "
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
        print(f"[p_v4/SCOUT] {wid}[{sc.mode}] wp={sc.waypoint_idx} "
              f"tgt={target} pos=({wx},{wy}) "
              f"d={d:.0f} start_d={sc.wp_start_dist:.0f}"
              f"{dd_recent} age={age}f "
              f"order={order_name(w['order'])} "
              f"bl={sorted(sc.blacklist)}")

        if d < ARRIVE_RADIUS:
            new_idx = next_wp_idx(sc, len(wps))
            reset_scout_wp(sc, new_idx, w, wps)
            sc.arrived_frame = frame
            print(f"[p_v4]  SCOUT {wid} [{sc.mode}] ARRIVED @{target}; "
                  f"next wp {new_idx} {wps[new_idx]}")
            try:
                nxt = wps[new_idx]
                await c.move(unit_id=wid, x=nxt[0], y=nxt[1])
            except Exception as e:
                print(f"[p_v4]  scout move error: {e}")
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
            print(f"[p_v4]  SCOUT {wid} [{sc.mode}] STUCK near "
                  f"({wx},{wy}) wp {sc.waypoint_idx}={target}; "
                  f"reason={stuck_reason}; blacklisting.")
            sc.blacklist.add(sc.waypoint_idx)
            new_idx = next_wp_idx(sc, len(wps))
            reset_scout_wp(sc, new_idx, w, wps)
            try:
                nxt = wps[new_idx]
                await c.move(unit_id=wid, x=nxt[0], y=nxt[1])
                print(f"[p_v4]  SCOUT {wid} [{sc.mode}] -> wp {new_idx} "
                      f"{nxt} (blacklist size {len(sc.blacklist)})")
            except Exception as e:
                print(f"[p_v4]  scout skip-move error: {e}")
        else:
            # Re-issue move only if the probe isn't already moving.
            order_name_str = order_name(w["order"])
            if order_name_str not in ("Move", "MoveToAttack",
                                      "AttackMove"):
                try:
                    await c.move(unit_id=wid, x=target[0], y=target[1])
                except Exception as e:
                    print(f"[p_v4]  scout re-move error: {e}")

    # 4) Harvest visibility -- remember enemies + off-base resources.
    for e in obs.get("enemies", []):
        if e.get("building") and e["unit_id"] not in known_enemies:
            known_enemies[e["unit_id"]] = KnownEnemy(
                unit_id=e["unit_id"], type_id=e["type"],
                x=e["x"], y=e["y"], first_seen_frame=frame)
            print(f"[p_v4]  SCOUT SPOTTED enemy building "
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
                print(f"[p_v4]  SCOUT SPOTTED {kind} @({n['x']},{n['y']}) "
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
# just pins new pylons back on top of the Nexus and undoes any spread
# the other strategies achieved.
_ANCHOR_STRATEGIES = ["toward_center", "furthest_own", "toward_center"]


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
    # toward_center: point along vector home -> map center
    cx, cy = map_w // 2, map_h // 2
    # random distance along the vector, [20%..90%] of the way to center.
    t = random.uniform(0.2, 0.9)
    ax = int(home_x + t * (cx - home_x))
    ay = int(home_y + t * (cy - home_y))
    # small jitter so we don't stack on the exact same line every time
    ax += random.randint(-160, 160)
    ay += random.randint(-160, 160)
    return (ax, ay)


# --------------------------------------------------------------------
# Expansion: find a mineral cluster far from all existing Nexuses.
# --------------------------------------------------------------------

# A cluster of mineral fields is considered "the same patch" when its
# fields sit within CLUSTER_MERGE_PX of each other. BW mineral fields
# at a base sit within ~200 px, so 400 gives generous merge.
CLUSTER_MERGE_PX = 400
# Ignore mineral clusters closer than this to any of our Nexuses --
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
    """Return (x, y) of a mineral cluster far from all our Nexuses.

    Skips clusters that have any Nexus (existing or in a pending
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
        # Reject clusters near any existing Nexus.
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
        print(f"[p_v4]  expand find_placement error: {e}")
        return None
    spots = resp.get("spots", [])
    if not spots:
        print(f"[p_v4]  EXPAND: no placement near cluster ({cx},{cy})")
        return None
    spot = spots[0]

    completed, ip = count_units(units, nexus_type)
    try:
        await c.build(unit_id=worker["unit_id"],
                      unit_type=nexus_type,
                      tile_x=spot["tile_x"], tile_y=spot["tile_y"])
    except Exception as e:
        print(f"[p_v4]  expand build cmd error: {e}")
        return None

    pending_expansion_pts.add((cx, cy))
    print(f"[p_v4] FIRE  EXPAND Nexus @cluster ({cx},{cy}) "
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
        print(f"[p_v4]  find_placement error {unit_type_name(spec.type_id)}: {e}")
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
        print(f"[p_v4]  build cmd error {unit_type_name(spec.type_id)}: {e}")
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
        print(f"[p_v4]  train worker error: {e}")
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
        print(f"[p_v4]  train unit error: {e}")
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
        print(f"[p_v4] FIRE  {spec.kind}:{spec.label} @{src['unit_id']} "
              f"cost={spec.cost_min}/{spec.cost_gas}")
        return True
    except Exception as e:
        print(f"[p_v4]  {spec.kind} error: {e}")
        return False


# --------------------------------------------------------------------
# Main loop.
# --------------------------------------------------------------------

async def run(c: Client, interval_sec: float,
              worker_target: int, supply_slack: int,
              worker_train_min: int, pylon_target: int,
              scout_radial: int, scout_zscan: int,
              base_target: int) -> None:
    print(f"[p_v4] connected slot={c.welcome.slot} "
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
    waypoints_by_mode: dict[str, list[tuple[int, int]]] = {
        "radial": [], "zscan": [],
    }
    target_by_mode = {"radial": scout_radial, "zscan": scout_zscan}

    home_x = home_y = 0
    tgt_x = tgt_y = None  # attack target

    # Anchor rotation counter (increments per catalog build attempt).
    anchor_strategy_idx = 0

    # Expansion state: which cluster centroids we've already committed
    # a Nexus build to. Prevents re-firing on the same cluster while a
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
            print(f"[p_v4] race={race} home=({home_x},{home_y}) "
                  f"map={map_w}x{map_h}")
            print(f"[p_v4] radial wps: {waypoints_by_mode['radial']}")
            print(f"[p_v4] zscan wps: {len(waypoints_by_mode['zscan'])} points "
                  f"({waypoints_by_mode['zscan'][:2]}...)")

        verify_pending(pending, obs, stats, grace_frames)

        # ---- Verify upgrades / tech via observation (server-authoritative).
        # obs["resources"]["upgrades"] = {upgrade_id: level}, level > 0 only.
        # obs["resources"]["tech"] = [tech_id, ...] for researched techs.
        # `upgrading`/`researching` are in-progress indicators (optional).
        obs_upgrades = r.get("upgrades", {})    # str-keyed by JSON
        obs_tech = set(r.get("tech", []))
        for spec in catalog_upgrades:
            key = (spec.kind, spec.enum_id)
            if key in completed_upgrades:
                continue
            if spec.kind == "upgrade":
                lvl = obs_upgrades.get(str(spec.enum_id), 0)
                if lvl > 0:
                    completed_upgrades.add(key)
                    print(f"[p_v4] TOOK  upgrade:{spec.label} "
                          f"(level {lvl})")
            else:  # research
                if spec.enum_id in obs_tech:
                    completed_upgrades.add(key)
                    print(f"[p_v4] TOOK  research:{spec.label}")

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
        # Fighter-parent summary: for each own completed Carrier or
        # Reaver, sum current fighter_count and fighter_max so the
        # status line shows "carriers 3 fighters=17/24" -- 17 babies
        # loaded, 24 possible if all carriers fully saturated.
        _CARRIER = UNIT_TYPES_BY_NAME["Protoss_Carrier"]
        _REAVER = UNIT_TYPES_BY_NAME["Protoss_Reaver"]
        fpc_have = fpc_max = fpr_have = fpr_max = 0
        for pu in units:
            if not pu.get("completed"): continue
            if pu["type"] == _CARRIER:
                fpc_have += pu.get("fighter_count", 0) + pu.get("fighter_queued", 0)
                fpc_max  += pu.get("fighter_max", 0)
            elif pu["type"] == _REAVER:
                fpr_have += pu.get("fighter_count", 0) + pu.get("fighter_queued", 0)
                fpr_max  += pu.get("fighter_max", 0)
        print(f"[p_v4] f={frame} min={r['minerals']} gas={r['gas']} "
              f"sup={r['supply_used']}/{r['supply_max']} "
              f"workers={n_workers}/{worker_target} "
              f"bases={nx_completed}(+{nx_in_progress})/{base_target} "
              f"pyl={pyl_c}(+{pyl_ip})/{pylon_target} "
              f"combat={n_combat} bldgs={n_bldgs} "
              f"btypes={b_types}/{len(catalog_buildings)} "
              f"utypes={u_types}/{len(catalog_units)} "
              f"upg={len(completed_upgrades)}(+{n_upg_inprog})/{len(catalog_upgrades)} "
              f"intcp={fpc_have}/{fpc_max} scarab={fpr_have}/{fpr_max} "
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
                            print(f"[p_v4] FIRE  build:Pylon "
                                  f"({pyl_total2 + 1}/{pylon_target}) "
                                  f"anchor={anchor_pt}")
                        else:
                            budget["min"] += 100  # refund
                    except Exception as e:
                        print(f"[p_v4]  pylon fire error: {e}")
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
                    print(f"[p_v4] FIRE  train:Probe ({n_workers + 1}/{worker_target})")
                else:
                    budget["min"] += worker_train_min

        # ---- Priority 4: gas structure.
        gas_bld = UNIT_TYPES_BY_NAME["Protoss_Assimilator"]
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
                    print(f"[p_v4] FIRE  build:Assimilator")
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
                print(f"[p_v4] FIRE  build:{unit_type_name(spec.type_id)}")
                catalog_build_this_tick += 1
            else:
                budget["min"] += spec.cost_min
                budget["gas"] += spec.cost_gas

        # ---- Priority 6: catalog units (up to 6 fires per tick,
        #      throttled per-type by target_count and pending grace).
        # Each UnitSpec carries target_count -- how many completed +
        # in-progress copies to maintain. Cheap Gateway units get big
        # targets (8) so an army actually forms; expensive Carrier /
        # Arbiter stay low (2 / 1) so they don't monopolise gas.
        # Pending grace keys per type_id so we don't refire the same
        # type before the sim shows the new unit in ip; different
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
                print(f"[p_v4] FIRE  train:{unit_type_name(spec.type_id)} "
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

        # ---- Priority 7.5: refill Carrier/Reaver fighters.
        # Each Carrier holds up to 8 Interceptors (25 min each) and
        # each Reaver up to 5 Scarabs (15 min each). The observation
        # includes fighter_count/fighter_queued/fighter_max per parent
        # so we fire train_fighter only when a specific parent needs
        # a refill -- no wasted commands. Cap at 3 fires per tick so
        # a mass of empty carriers doesn't yank all the minerals in
        # one tick.
        FIGHTER_COST_MIN = {
            UNIT_TYPES_BY_NAME["Protoss_Carrier"]: 25,
            UNIT_TYPES_BY_NAME["Protoss_Reaver"]: 15,
        }
        fighter_fires = 0
        for pu in units:
            if fighter_fires >= 3:
                break
            if not pu.get("completed"):
                continue
            cost = FIGHTER_COST_MIN.get(pu["type"])
            if cost is None:
                continue
            fc = pu.get("fighter_count", 0)
            fq = pu.get("fighter_queued", 0)
            fm = pu.get("fighter_max", 0)
            if fc + fq >= fm:
                continue
            if not reserve(cost, 0):
                continue
            try:
                await c.train_fighter(unit_id=pu["unit_id"])
                print(f"[p_v4] FIRE  train_fighter "
                      f"{unit_type_name(pu['type'])} {pu['unit_id']} "
                      f"({fc}+{fq}/{fm})")
                fighter_fires += 1
            except Exception as e:
                print(f"[p_v4]  train_fighter error: {e}")
                budget["min"] += cost

        # ---- Priority 8: attack (idle combat -> nearest known enemy).
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
                # Else attack-move toward known enemy base (or fallback).
                if tgt_x is not None:
                    await c.attack(unit_id=u["unit_id"], target_unit=0,
                                   x=tgt_x, y=tgt_y)
            except Exception as e:
                print(f"[p_v4]  attack error: {e}")

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
                    print(f"[p_v4]  cover move error: {e}")
        if move_done and not stop_done:
            cands = [u for u in units if u["order"] not in IDLE_ORDERS
                     and not u.get("building")]
            if cands:
                try:
                    await c.stop(unit_id=cands[0]["unit_id"])
                    stop_done = True
                except Exception as e:
                    print(f"[p_v4]  cover stop error: {e}")

        await asyncio.sleep(interval_sec)


async def main(api_key, host, port, interval_sec, worker_target,
               supply_slack, worker_train_min, pylon_target,
               scout_radial, scout_zscan, base_target):
    async with Client(api_key=api_key, host=host, port=port) as c:
        await run(c, interval_sec, worker_target, supply_slack,
                  worker_train_min, pylon_target,
                  scout_radial, scout_zscan, base_target)


def entrypoint() -> None:
    p = argparse.ArgumentParser(
        prog="python3 -m python_agent.agents.p_agent_v4",
        description="v2 + patrol + wider building spread + upgrades + expansions.")
    p.add_argument("api_key")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=6113)
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
                   help="target total Nexus count including main base")
    args = p.parse_args()
    try:
        asyncio.run(main(args.api_key, args.host, args.port,
                         args.interval_sec, args.worker_target,
                         args.supply_slack, args.worker_train_min,
                         args.pylon_target,
                         args.scout_radial, args.scout_zscan,
                         args.base_target))
    except KeyboardInterrupt:
        print("\n[p_v4] stopped")


if __name__ == "__main__":
    entrypoint()
