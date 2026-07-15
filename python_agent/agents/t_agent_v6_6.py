"""t_agent_v6_6: v6_5 + stale-siege unsiege for the tank arc.

v6_5's rotate-forward step advance unsieged half the tanks per
step. That works when tanks are spread across the arc, but in
practice we observed the front-row tanks stay sieged at an OLD
anchor position while the back-row tries to move to the NEW arc
positions. The moving tanks pathed AROUND the sieged tanks and got
blocked -- and because defense_line_ready requires ALL tanks
within DEFENSE_LINE_TIGHT_PX to allow the next step, the whole
line stalled.

Changes vs. v6_5:
  * Stale-siege unsiege: `tank_siege_start_frame[uid]` tracks how
    long each tank has been in Siege_Mode. If a tank has been
    sieged >= STALE_SIEGE_FRAMES AND its position is >
    TANK_SLOT_TOLERANCE_PX from its assigned slot on the CURRENT
    anchor, we unsiege it so it can walk forward. Counter resets
    when the tank enters siege again at its new slot.
  * Quorum-based line-ready: `defense_line_ready` now returns True
    when >= 60 % of completed tanks are within
    DEFENSE_LINE_TIGHT_PX, not 100 %. One stuck tank no longer
    blocks the whole line.

Everything else is v6_5 verbatim.

--- inherited notes from v6_5: ---

t_agent_v6_5: v6_4 + Factory room + sustained workers, minus load.

v6_4 showed three real problems in play:
  * Machine Shop attach kept forcing Factory lift-to-relocate.
    Root cause: Factory was placed near the Command Center with
    a small radius, so the +2 tile addon slot east of it was
    blocked by neighbouring buildings. Without Machine Shop,
    Siege Mode tech never researches and tanks stay Tank_Mode
    forever (worse yet, production stalls while the Factory is
    airborne).
  * Once workers died in combat, they didn't get replaced. n_workers
    fell 40 -> 5 without any log line explaining why train:SCV
    wasn't firing. Diagnosis added in v6_5.
  * Bunker `load` fires spam every tick because the observation
    doesn't tell us who's already inside (see
    issues/2026-07-14-observation-missing-bunker-load-state.md).
    Skipping the load pass entirely for now per user; marines
    revert to the v6_2/v6_3 "patrol ring near the bunker" behaviour.

Changes vs. v6_4:
  * Factory placement: dedicated anchor `factory_anchor(...)` at
    60 % of the way from home toward the current defense anchor
    (or toward map center if no anchor yet). find_placement
    radius bumped to 20 tiles for Factory specifically. Result:
    Factories land in open ground with 2 tiles of clear space to
    their east so Machine Shop can attach on the first try.
  * Machine Shop attach guard: try_addon on Factory validates
    that the east-adjacent 2-tile block is clear BEFORE firing;
    otherwise skip and let lift-to-safety re-park it. Prevents
    the retry loop that kept mineral-locked builds pending.
  * SCV maintenance: worker training pass now always fires
    when n_workers < worker_target AND a completed CC exists
    that isn't training. Logs a WORKER-STALL line each 500
    frames if we would like to train but a check fails.
  * Load pass removed. `phase_defense_line` pass (4) reverts to
    the v6_3 "move marine to a ring near the bunker" pattern.
    No c.load() call. Same for `phase_base_garrison` marine
    dispatch — they walk to the CC / bunker area and stand
    there, ready to shoot.

--- inherited notes from v6_4: ---

t_agent_v6_4: v6_3 + bunker garrison + tank arc + auto-expand
+ per-base defense.

Server team shipped the `load` verb (Orders::EnterTransport),
so v6_4 finally garrisons bunkers with Marines instead of leaving
them milling around outside. Combined with a rotating tank arc,
per-base defense at each expansion, and a resource-driven trigger
for further expansions.

Changes vs. v6_3:
  * Bunker load: `phase_defense_line` pass (4) now issues
    `c.load(marine_id, bunker_id)` when a bunker has < 4
    passengers (`transport_id == bunker_id` in the observation
    tells us who's already inside). Marines still get sent to
    the ring first as a staging area; once at the ring, next
    tick's load fires them into the bunker.
  * Tank arc: each completed tank is assigned a slot index on
    a semi-circle around the current anchor (angle 90..270 deg
    facing the enemy vector, spacing 128 px). No more single-tile
    tank pile. When defense_step advances, only ODD-indexed
    tanks unsiege and walk to new slots; the even-indexed tanks
    stay sieged as cover. Next advance flips the roles. Rotate-
    forward instead of everyone-unsiege-together.
  * Tank target 6 -> 10 in TERRAN_UNITS; Factory target 2 -> 3
    so production keeps up.
  * Per-base garrison: for each expansion beyond the main base,
    build 1 Bunker + 1 Missile Turret at the CC, dispatch 2
    Marines (loaded into the bunker) + 1 Tank (sieged nearby)
    when available. Tracked per-CC in `base_garrison` state.
  * Auto-expand: monitors sum of `resources[m]["minerals"]` for
    mineral fields within RESOURCE_NEAR_CC_PX of any own CC.
    When total < 20% of the initial total OR any base is
    "exhausted" (near-zero nearby minerals), next expansion is
    triggered even if base_target hasn't been reached. Cap is
    now `max(base_target, actual_needed)`.
  * SERVER_ASKS.md removed -- the two ask verbs (load, unload)
    are now in the protocol.

--- inherited notes from v6_3: ---

t_agent_v6_3: v6_2 + expansion safety + turrets along the line.

v6_2 opened well but lost the game later:
  * Second base was built once (NE cluster), but the CC lifted
    under attack and never re-landed. After the CC lifted, the
    expansion loop picked the WEST cluster (77, 2384) -- enemy
    adjacent -- and fed SCV after SCV to their death. This
    happened because pick_expansion_site's `rest` fallback
    accepts ANY cluster when no our-side cluster is currently
    valid, and try_expand's SCV picker chose whichever worker
    was already closest to the destination (i.e. whichever was
    halfway to the enemy).
  * Only 1 Siege Tank ever completed. Factories got lifted to
    make room for Machine Shop, which worked, but later Factories
    were lifted for HP damage and never re-landed, so training
    stopped.
  * Missile Turrets covered only the initial anchor and home.
    When the defense line stepped forward (0.10 -> 0.70), the
    tanks and marines walked past the turret ring into
    uncovered territory; anything invisible killed them.

Changes vs. v6_2:
  * pick_expansion_site: when we've seen any enemy building, we
    NEVER accept an enemy-side cluster. If no our-side cluster
    exists, expansion is skipped this tick instead of firing at
    a suicide spot.
  * try_expand: picks the SCV nearest to HOME, not to the
    destination cluster. Keeps the walk short and safer.
  * Cluster blacklist: each cluster tracks a REJECT counter.
    After 3 consecutive REJECTs the cluster is permanently
    excluded from expansion consideration. Prevents the same
    doomed target from consuming a stream of SCVs.
  * Turret chain: every time defense_step advances, the OLD
    anchor is recorded in `defense_line_history`. phase_defense_line
    ensures each historical anchor has a Missile Turret nearby
    (within 200 px). Result: a chain of detection along the
    entire home -> current-anchor path so invisible attackers
    can't slip through.

--- inherited notes from v6_2: ---

t_agent_v6_2: v6_1 + army-first opening.

v6_1 collapsed at ~15k frames because it invested minerals in the
Bunker / Engineering Bay / Barracks / Academy simultaneously and
had no combat units when the first Protoss push arrived. The SCV
sent to build the anchor Bunker kept getting killed en route.

v6_2 changes the OPENING so the defensive line comes AFTER the
first batch of Marines is out. Late-game passes are unchanged.

Changes vs. v6_1:
  * Army-first gate: phase_defense_line is a no-op until
    `opening_complete = (barracks_completed >= 1
                         AND marines_completed >= --opening-marine-target)`.
    Default marine target 6. The 100 min that would have gone to
    the first Bunker instead trains Marines.
  * Scout: --scout-zscan defaults to 1 (single whole-map scout,
    unchanged), but --scout-radial defaults to 0. Only one SCV
    on scout duty during opening. Everyone else mines.
  * Everything post-opening is identical to v6_1: mining-corridor
    guard, defense line at anchor, expansion after >= 1 Bunker,
    push-forward mode, etc.

Rationale (from a losing v6_1 game):
  - v6_1 fires Bunker + Barracks + Academy + Eng Bay in the same
    ~5 tick window. Each grabs 100..150 min. Meanwhile Marine
    training doesn't start until the Barracks is complete (~600
    frames from fire). By the time the first Marine trains, the
    Protoss zealot batch is already at the choke.
  - v6_2 sequences: Barracks -> Marines x6 -> then Bunker / Eng
    Bay / Turret. The Bunker anchor SCV walks past friendly
    Marines instead of walking past no one.

--- inherited notes from v6_1: ---

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
    # v6_2: True = allowed during the opening (before the first
    # marine batch is out). Only Barracks is opening_ok in the
    # catalog; everything else waits. Refinery has its own
    # dedicated Priority 4 fire and is always allowed.
    opening_ok: bool = False

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
    BuildingSpec(UNIT_TYPES_BY_NAME["Terran_Barracks"],           150,   0, "cc", target_count=2, opening_ok=True),
    BuildingSpec(UNIT_TYPES_BY_NAME["Terran_Engineering_Bay"],   125,   0, "cc"),
    # v6_5: Factory uses a dedicated "factory" anchor further out
    # from the CC (see pick_factory_anchor) so the Machine Shop
    # addon has room to attach on the east side. In v6_4 Factories
    # placed near the CC had their addon slot blocked by other
    # buildings and had to lift-to-relocate, stalling production.
    BuildingSpec(UNIT_TYPES_BY_NAME["Terran_Factory"],           200, 100, "factory", target_count=3),
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
    # v6_4: 10 tanks fill the semi-circular arc around the anchor
    # (TANK_ARC_SLOTS). Combined with Factory target 3 (below),
    # this actually gets produced in a reasonable time. Prior
    # versions targeted 6 but rejects kept re-firing the same
    # slot; the display "1/6" was misleading -- v6_4 fixes it.
    UnitSpec(UNIT_TYPES_BY_NAME["Terran_Siege_Tank_Tank_Mode"],
             UNIT_TYPES_BY_NAME["Terran_Factory"],        150, 100, 2, target_count=10),
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
    raise SystemExit(f"[t_v6_6] race={race} not supported (this is the "
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
            print(f"[t_v6_6] TOOK  {p.label():48s} "
                  f"(count {p.pre_count}->{cur_count} after {age}f)")
            to_drop.append(key)
            continue
        if age >= grace_frames:
            stats.reject[p.label()] += 1
            n = stats.reject[p.label()]
            if n == 1 or n % 5 == 0:
                print(f"[t_v6_6] REJECT {p.label():48s} after {age}f. n={n}. "
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
                    print(f"[t_v6_6]  gather-gas error: {e}")

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
                print(f"[t_v6_6]  gather-min error: {e}")

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
            print(f"[t_v6_6]  SCOUT worker {wid} died; unassigning")
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
            print(f"[t_v6_6]  SCOUT worker {w['unit_id']} mode={mode} "
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
            print(f"[t_v6_6]  SCOUT {sc.worker_id} [{sc.mode}] all "
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
        print(f"[t_v6_6/SCOUT] {wid}[{sc.mode}] wp={sc.waypoint_idx} "
              f"tgt={target} pos=({wx},{wy}) "
              f"d={d:.0f} start_d={sc.wp_start_dist:.0f}"
              f"{dd_recent} age={age}f "
              f"order={order_name(w['order'])} "
              f"bl={sorted(sc.blacklist)}")

        if d < ARRIVE_RADIUS:
            new_idx = next_wp_idx(sc, len(wps))
            reset_scout_wp(sc, new_idx, w, wps)
            sc.arrived_frame = frame
            print(f"[t_v6_6]  SCOUT {wid} [{sc.mode}] ARRIVED @{target}; "
                  f"next wp {new_idx} {wps[new_idx]}")
            try:
                nxt = wps[new_idx]
                await c.move(unit_id=wid, x=nxt[0], y=nxt[1])
            except Exception as e:
                print(f"[t_v6_6]  scout move error: {e}")
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
            print(f"[t_v6_6]  SCOUT {wid} [{sc.mode}] STUCK near "
                  f"({wx},{wy}) wp {sc.waypoint_idx}={target}; "
                  f"reason={stuck_reason}; blacklisting.")
            sc.blacklist.add(sc.waypoint_idx)
            new_idx = next_wp_idx(sc, len(wps))
            reset_scout_wp(sc, new_idx, w, wps)
            try:
                nxt = wps[new_idx]
                await c.move(unit_id=wid, x=nxt[0], y=nxt[1])
                print(f"[t_v6_6]  SCOUT {wid} [{sc.mode}] -> wp {new_idx} "
                      f"{nxt} (blacklist size {len(sc.blacklist)})")
            except Exception as e:
                print(f"[t_v6_6]  scout skip-move error: {e}")
        else:
            # Re-issue move only if the probe isn't already moving.
            order_name_str = order_name(w["order"])
            if order_name_str not in ("Move", "MoveToAttack",
                                      "AttackMove"):
                try:
                    await c.move(unit_id=wid, x=target[0], y=target[1])
                except Exception as e:
                    print(f"[t_v6_6]  scout re-move error: {e}")

    # 4) Harvest visibility -- remember enemies + off-base resources.
    for e in obs.get("enemies", []):
        if e.get("building") and e["unit_id"] not in known_enemies:
            known_enemies[e["unit_id"]] = KnownEnemy(
                unit_id=e["unit_id"], type_id=e["type"],
                x=e["x"], y=e["y"], first_seen_frame=frame)
            print(f"[t_v6_6]  SCOUT SPOTTED enemy building "
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
                print(f"[t_v6_6]  SCOUT SPOTTED {kind} @({n['x']},{n['y']}) "
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

# v6_5: when placing a Factory, we want the east-side 2x2 addon
# slot to be clear so a Machine Shop can attach without lifting.
# 96 px = 3 tiles; Machine Shop is 2x2 tiles, so 3 tiles of
# clearance guarantees the 2x2 slot is unobstructed.
FACTORY_ADDON_CLEAR_PX = 96

# v6_5: addon retry cadence.
# ADDON_RETRY_FRAMES: min frames between PlaceAddon fires on the
#   same parent building. 500 = ~5 sec at game-speed 10.
# ADDON_MAX_RETRIES: after this many failed fires, give up on
#   placement and lift the parent to relocate to open ground.
# ADDON_MATCH_PX: how close an addon unit must be to its parent
#   for us to consider "this parent already has its addon". BW
#   places addons at +2 tile offset (64-96 px), so 150 gives a
#   safe margin.
ADDON_RETRY_FRAMES = 500
ADDON_MAX_RETRIES = 3
ADDON_MATCH_PX = 150


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

# v6_4: a mineral field is "near" a CC (i.e. that CC's field)
# if within this many pixels. 500 = ~15 tiles; captures the
# typical BW resource cluster around a base without picking
# up minerals at the next-door expansion.
RESOURCE_NEAR_CC_PX = 500

# A base is considered "depleted" when it has <= this many
# mineral fields left within RESOURCE_NEAR_CC_PX. 2 = below
# saturable mining rate; time to build the next CC.
DEPLETION_MINERAL_COUNT = 2

# v6_3: how many consecutive expansion REJECTs on the same cluster
# before we blacklist it. 3 is enough to distinguish "unlucky
# find_placement" (usually 1 retry succeeds) from "the enemy owns
# this ground and every SCV is dying" (10+ REJECTs in v6_2).
EXPANSION_REJECT_THRESHOLD = 3


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
                        cluster_blacklist: set[tuple[int, int]] | None = None,
                        ) -> tuple[int, int] | None:
    """Return (x, y) of a mineral cluster far from all our CCs.

    Skips clusters that have any CC (existing or in a pending
    expansion) within MIN_EXPANSION_DIST_PX. Prefers clusters with more
    mineral fields (already sorted by cluster_resources).

    v6_3 behaviour:
      * When we've seen ANY enemy building, only OUR-SIDE clusters
        (d_home < d_enemy) are candidates. No fallback to the rest.
        v6_2's fallback let SCV after SCV march to a suicide cluster
        on the enemy's side of the map.
      * When `cluster_blacklist` is provided, clusters within
        MIN_EXPANSION_DIST_PX of any blacklisted centroid are
        skipped entirely. Fed by the try_expand REJECT counter.
    """
    if not known_resources or not own_nexuses:
        return None
    if cluster_blacklist is None:
        cluster_blacklist = set()
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
            # No enemy seen yet -- everything is a candidate.
            our_side.append(c)

    def _first_valid(cands: list[tuple[int, int, int, int]]
                     ) -> tuple[int, int] | None:
        for cx, cy, n_min, n_gas in cands:
            if n_min < 4:
                continue
            # v6_3: skip blacklisted clusters. The blacklist stores
            # centroids of clusters that have racked up REJECTs
            # in try_expand.
            too_close = False
            for bx, by in cluster_blacklist:
                if dist_pixels(cx, cy, bx, by) < MIN_EXPANSION_DIST_PX:
                    too_close = True
                    break
            if too_close:
                continue
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

    # v6_3: our-side only. No `rest` fallback -- if no our-side
    # cluster is valid, we skip expansion this tick and try again
    # next tick with (hopefully) a different resource picture.
    return _first_valid(our_side)


async def try_expand(c: Client, obs: dict,
                     nexus_type: int, worker_type: int,
                     known_resources: dict[int, tuple[int, int, int]],
                     pending_expansion_pts: set[tuple[int, int]],
                     busy_workers: set[int],
                     known_enemies: dict | None = None,
                     home_x: int = 0, home_y: int = 0,
                     cluster_blacklist: set[tuple[int, int]] | None = None,
                     ) -> tuple[Pending, tuple[int, int]] | None:
    """v6_3: returns (Pending, cluster_centroid) so the caller can
    track cluster-specific REJECT counters and blacklist chronic
    failure spots. Also picks the SCV nearest to HOME (not to the
    cluster) so the worker starts its walk from a safer position."""
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
                               home_x=home_x, home_y=home_y,
                               cluster_blacklist=cluster_blacklist)
    if site is None:
        return None
    cx, cy = site

    # v6_3: nearest probe to HOME (not to the cluster). Prevents
    # the "worker halfway to the enemy gets picked and walks the
    # rest of the way to its death" pattern that broke v6_2.
    cands = [u for u in workers(units)
             if u["type"] == worker_type
             and u["unit_id"] not in busy_workers]
    if not cands:
        return None
    worker = min(cands, key=lambda u: dist_pixels(u["x"], u["y"], home_x, home_y))

    try:
        resp = await c.find_placement(
            unit_type=nexus_type,
            worker_unit=worker["unit_id"],
            center_x=cx, center_y=cy,
            radius_tiles=8, max_results=8)
    except Exception as e:
        print(f"[t_v6_6]  expand find_placement error: {e}")
        return None
    spots = resp.get("spots", [])
    if not spots:
        print(f"[t_v6_6]  EXPAND: no placement near cluster ({cx},{cy})")
        return None
    spot = spots[0]

    completed, ip = count_units(units, nexus_type)
    try:
        await c.build(unit_id=worker["unit_id"],
                      unit_type=nexus_type,
                      tile_x=spot["tile_x"], tile_y=spot["tile_y"])
    except Exception as e:
        print(f"[t_v6_6]  expand build cmd error: {e}")
        return None

    pending_expansion_pts.add((cx, cy))
    print(f"[t_v6_6] FIRE  EXPAND CC @cluster ({cx},{cy}) "
          f"tile=({spot['tile_x']},{spot['tile_y']}) worker={worker['unit_id']}")
    pending = Pending(
        verb="build", target_type=nexus_type,
        issued_frame=obs["current_frame"],
        pre_min=r["minerals"], pre_gas=r["gas"],
        pre_count=completed + ip,
        cost_min=400, cost_gas=0,
        worker_id=worker["unit_id"],
    )
    return pending, (cx, cy)


# --------------------------------------------------------------------
# try_build with anchor strategy hook.
# --------------------------------------------------------------------

async def try_build(c: Client, obs: dict, spec: BuildingSpec,
                    worker_type: int, main_type: int, supply_type: int,
                    pending_workers: set[int],
                    home_x: int, home_y: int,
                    map_w: int, map_h: int,
                    anchor_strategy_idx: int,
                    factory_anchor_hint: tuple[int, int] | None = None,
                    ) -> Pending | None:
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
    elif spec.anchor == "factory":
        # v6_5: Factory anchor. Aim between the base and the
        # front defense anchor (60 % of the way), so:
        #   * we're outside the tight main-base cluster (~12 tile
        #     radius from CC) where addon slots are usually blocked
        #   * still on our side, protected by the defense arc
        # Fall back to `pick_anchor` if the caller didn't pass a
        # hint (e.g. defense_step is 0 and we don't have a real
        # front line yet).
        if factory_anchor_hint is not None:
            kwargs["center_x"], kwargs["center_y"] = factory_anchor_hint
        else:
            anchor_pt = pick_anchor(units, main_type,
                                    anchor_strategy_idx,
                                    home_x, home_y, map_w, map_h)
            if anchor_pt is not None:
                kwargs["center_x"], kwargs["center_y"] = anchor_pt
        # Wider search radius so more east-side spots are found and
        # the Machine Shop addon slot is more likely to be clear.
        kwargs["radius_tiles"] = 20
        kwargs["max_results"] = 16
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
        print(f"[t_v6_6]  find_placement error {unit_type_name(spec.type_id)}: {e}")
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
            print(f"[t_v6_6]  build {unit_type_name(spec.type_id)}: "
                  f"all {len(spots)} spots inside mining corridor; "
                  f"skipping this tick")
            return None
        # v6_5: Factory addon-slot guard. Machine Shop attaches at
        # offset (+4 tiles x, +1 tile y) from the Factory tile
        # origin. Factory footprint is 4 tiles wide, 3 tiles tall.
        # Reject a Factory spot whose east side (the 2x2 block
        # starting 4 tiles east of the origin at y+1..y+2) has
        # another building within FACTORY_ADDON_CLEAR_PX = 96 px
        # (= 3 tiles).
        # This lets Machine Shop attach on the first try instead of
        # forcing a lift-to-relocate. If ALL spots fail this check,
        # fall back to the corridor-only filter (better to place a
        # Factory that needs relocation than not build one at all).
        if spec.anchor == "factory":
            keep_addon = []
            # Precompute the set of existing building centers we
            # want to avoid overlapping with the addon slot.
            existing_buildings = [
                (u["x"], u["y"]) for u in units
                if u.get("building") and u.get("completed") is not False
            ]
            for s in keep:
                # Origin tile of factory = spot["tile_x/y"]. Addon
                # slot origin in pixels = (tile_x+4) * 32,
                # (tile_y+1) * 32. Compute the center of that 2x2
                # block for a coarse clearance check.
                ax_center = (s["tile_x"] + 5) * 32
                ay_center = (s["tile_y"] + 2) * 32
                blocked = False
                for bx, by in existing_buildings:
                    if dist_pixels(ax_center, ay_center, bx, by) < FACTORY_ADDON_CLEAR_PX:
                        blocked = True
                        break
                if not blocked:
                    keep_addon.append(s)
            if keep_addon:
                spot = keep_addon[0]
                print(f"[t_v6_6]  build {unit_type_name(spec.type_id)}: "
                      f"picked spot ({spot['tile_x']},{spot['tile_y']}) "
                      f"with clear addon slot")
            else:
                # No perfectly-clear addon spot; fall back to the
                # best corridor-safe spot rather than skip entirely.
                spot = keep[0]
                print(f"[t_v6_6]  build {unit_type_name(spec.type_id)}: "
                      f"NO clear-addon spot in {len(keep)} candidates; "
                      f"placing anyway at ({spot['tile_x']},{spot['tile_y']})")
        else:
            spot = keep[0]
    else:
        spot = spots[0]

    completed, ip = count_units(units, spec.type_id)
    try:
        await c.build(unit_id=worker["unit_id"], unit_type=spec.type_id,
                      tile_x=spot["tile_x"], tile_y=spot["tile_y"])
    except Exception as e:
        print(f"[t_v6_6]  build cmd error {unit_type_name(spec.type_id)}: {e}")
        return None
    return Pending(
        verb="build", target_type=spec.type_id,
        issued_frame=obs["current_frame"],
        pre_min=r["minerals"], pre_gas=r["gas"],
        pre_count=completed + ip,
        cost_min=spec.cost_min, cost_gas=spec.cost_gas,
        worker_id=worker["unit_id"],
    )


async def try_train_worker(c, obs, worker_type, main_type, cost_min,
                            exclude_cc_ids: set[int] | None = None):
    """v6_5: fire an SCV train at any completed, non-flying CC that
    isn't already excluded (used to avoid firing at the same CC
    twice in one tick). Returns the Pending on success plus the
    CC unit_id we fired at, so the caller can add it to
    exclude_cc_ids for the next fire in the same tick.

    Silent-rejects when no CC is available or minerals fall short.
    """
    r = obs["resources"]
    if r["minerals"] < cost_min:
        return None
    exclude_cc_ids = exclude_cc_ids or set()
    mains = [m for m in own_of_type(obs["units"], main_type)
             if not m.get("flying")
             and m["unit_id"] not in exclude_cc_ids]
    if not mains:
        return None
    # Pick any CC; scan-order (stable) is fine, no need for
    # sophisticated queue-length inspection since the sim rejects
    # if the CC is busy and we retry next tick.
    p = mains[0]
    completed, ip = count_units(obs["units"], worker_type)
    try:
        await c.train(unit_id=p["unit_id"], unit_type=worker_type)
    except Exception as e:
        print(f"[t_v6_6]  train worker error: {e}")
        return None
    return (Pending(verb="train", target_type=worker_type,
                    issued_frame=obs["current_frame"],
                    pre_min=r["minerals"], pre_gas=r["gas"],
                    pre_count=completed + ip,
                    cost_min=cost_min, cost_gas=0),
            p["unit_id"])


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
        print(f"[t_v6_6]  train unit error: {e}")
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
# A completed tank counts as "on the line" when within this many
# pixels of the current anchor. Used both by defense_line_ready
# (step-advance gate) and the arc slot pass.
DEFENSE_LINE_TIGHT_PX = 400
# v6_6: fraction of completed tanks required within
# DEFENSE_LINE_TIGHT_PX for defense_line_ready to return True.
# Was 100 % (all tanks); one stuck tank stalled the whole line.
# 60 % lets a couple of stragglers exist while the majority
# advances; stale-siege unsiege reels them in.
LINE_READY_QUORUM = 0.60
# v6_6: a tank counted as "stale-sieged" if it's been in Siege_Mode
# for at least this many frames AND its position is more than
# TANK_SLOT_TOLERANCE_PX from its assigned slot. Stale-sieged
# tanks get unsieged so they can walk to the new slot. 2000f =
# ~20s at game-speed 10 -- long enough to finish natural
# retargeting, short enough that a moved line doesn't stall.
STALE_SIEGE_FRAMES = 2000
# "Enemy nearby" radius used to VETO a step-forward in defensive
# mode (ignored in push mode).
DEFENSE_ENEMY_VETO_PX = 700
# Push-mode trigger: >= this many completed tanks near the current
# anchor and we start pushing the line forward.
PUSH_TANK_THRESHOLD = 3
# Marines patrol this far from the nearest bunker; keeps them within
# firing range while (until the server ships a `load` verb) actually
# entering the bunker isn't available. Retained for legacy pass; the
# actual load range is MARINE_BUNKER_LOAD_PX below.
MARINE_BUNKER_RING_PX = 200

# v6_4: fire c.load when the marine is within this many pixels of
# the bunker. BW EnterTransport auto-walks the marine to the bunker
# so this doesn't strictly need to be small, but firing load from
# very far away is wasteful (the marine will path there anyway and
# just re-fire is fine). 300 px = ~9 tiles; comfortably inside the
# marine's sight range.
MARINE_BUNKER_LOAD_PX = 300

# v6_3: a historical defense-line anchor is considered "covered" if
# there's already a Missile Turret within this many pixels of it.
# 200 px = about the same as MARINE_BUNKER_RING_PX; turret detection
# range is 7 tiles (224 px) so anything within 200 px overlaps well.
DEFENSE_TURRET_COVER_PX = 200

# v6_4: an expansion's garrison structure (Bunker / Missile Turret)
# is considered "at the base" if within this many pixels of the CC.
# 300 px = ~9 tiles; a Bunker + a Turret comfortably fit in this
# range without blocking the CC's mineral path.
GARRISON_STRUCTURE_PX = 300

# v6_4 tank arc: radius from the anchor and slot count. 10 slots
# across a 180-degree arc gives 20-degree spacing at 320 px radius
# = about 112 px between slot centers, which is bigger than a
# Siege Tank footprint (60 px) so tanks won't overlap.
TANK_ARC_RADIUS_PX = 320
TANK_ARC_SLOTS = 10
# A tank is considered "at its slot" if within this many pixels.
TANK_SLOT_TOLERANCE_PX = 80

# v6_2: Priority 8 "attack nearest visible enemy" is restricted to
# enemies within this radius of the attacking unit. 800 px = 25 tiles,
# well beyond any unit's sight range (Marine 7, Ghost 9, Battlecruiser
# 11). Wide enough that a marine still responds when a bunker two
# doorways over is under fire, but capped so the scout SCV sharing
# sight of the enemy BASE (typically 60+ tiles away) doesn't drag
# the whole army across the map.
# Bumped from 320 (v6_2 initial) after observing marines idle while
# nearby friendlies took damage from targets 12-20 tiles away.
ATTACK_HOLD_RADIUS_PX = 800


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


def tank_slot_position(anchor_x: int, anchor_y: int,
                       home_x: int, home_y: int,
                       slot_idx: int,
                       total_slots: int = TANK_ARC_SLOTS,
                       radius_px: int = TANK_ARC_RADIUS_PX
                       ) -> tuple[int, int]:
    """v6_4: (x, y) for the given tank slot on the semi-circular arc
    around the anchor. The arc faces the ENEMY (away from home).

    slot_idx=0 is one end of the arc, slot_idx=total_slots-1 is the
    other end. The middle slot sits on the anchor->enemy line.

    Direction convention: enemy is anywhere from home. We compute
    the forward vector (home -> anchor), normalize it, then spread
    slots across the perpendicular arc facing that direction.
    """
    # Forward unit vector (home -> anchor). If the anchor sits on
    # home (defense_step == 0 or degenerate), pick an arbitrary
    # forward direction; the caller shouldn't be placing tanks
    # there anyway.
    fx = anchor_x - home_x
    fy = anchor_y - home_y
    mag = math.hypot(fx, fy)
    if mag < 1.0:
        forward_angle = 0.0
    else:
        forward_angle = math.atan2(fy, fx)
    # Slots span [-pi/2, +pi/2] radians (180 deg arc) around
    # forward. slot 0 = -pi/2, slot N-1 = +pi/2.
    if total_slots <= 1:
        rel = 0.0
    else:
        rel = -math.pi / 2 + math.pi * (slot_idx / (total_slots - 1))
    slot_angle = forward_angle + rel
    sx = int(anchor_x + math.cos(slot_angle) * radius_px)
    sy = int(anchor_y + math.sin(slot_angle) * radius_px)
    return (sx, sy)


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
        print(f"[t_v6_6]  defense find_placement error "
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
        print(f"[t_v6_6]  defense build cmd error "
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
                             turret_target: int,
                             opening_complete: bool,
                             defense_line_history: list[tuple[int, int]],
                             tank_slot: dict[int, int],
                             ) -> None:
    """v6_3 defense pass (extends v6_2):

    Same passes as v6_2 plus a fifth: turrets along the historical
    defense line. Each time defense_step advances, the caller
    appends the OLD anchor to `defense_line_history`. This pass
    ensures every point in that history has a Missile Turret
    within DEFENSE_TURRET_COVER_PX (200 px) of it. Result: an
    unbroken detection chain from home to the current anchor,
    protecting the whole path from cloaked/invisible threats.
    Rate-limit: at most one gap turret per tick (piggybacks on the
    base turret pass's 1-per-tick budget).

    v6_2 addition (still in effect): `opening_complete` gate.
    Until the caller reports the opening batch of Marines is out,
    this pass is a no-op.

    Passes when the gate is open:
      (1) Ensure `bunker_target` bunkers near the defense anchor.
      (2) Ensure `turret_target` Missile Turrets, spread half at
          the anchor and half at home for anti-air / anti-cloak
          coverage on both surfaces.
      (2b) v6_3: fill turret gaps along `defense_line_history`.
      (3) Move completed Siege Tanks (Tank_Mode) more than
          DEFENSE_LINE_TIGHT_PX from the anchor onto the anchor.
      (4) Marines patrol a ring near each bunker.
    """
    if not opening_complete:
        return
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
            print(f"[t_v6_6] FIRE  build:Bunker "
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
            print(f"[t_v6_6] FIRE  build:Missile_Turret "
                  f"({tur_c + tur_ip + 1}/{turret_target}) "
                  f"@{'anchor' if use_anchor else 'home'}=({tx},{ty})")

    # (2b) v6_3: fill turret gaps along the historical defense line.
    # For each point in `defense_line_history`, if there's no
    # completed OR in-progress Missile Turret within
    # DEFENSE_TURRET_COVER_PX of it, queue one. We only fire one
    # gap turret per tick (piggybacks on base turret pass's
    # 1-per-tick budget) so we don't burn 4x75 min on one call.
    if (eb_c > 0
            and defense_line_history
            and f"build:{_TURRET}" not in pending
            and reserve(75, 0)):
        # Existing turret positions (completed + in-progress).
        turrets_now = [u for u in units if u["type"] == _TURRET]
        gap_pt: tuple[int, int] | None = None
        for hx, hy in defense_line_history:
            covered = False
            for t in turrets_now:
                if dist_pixels(hx, hy, t["x"], t["y"]) < DEFENSE_TURRET_COVER_PX:
                    covered = True
                    break
            if not covered:
                gap_pt = (hx, hy)
                break
        if gap_pt is not None:
            gx, gy = gap_pt
            p = await try_build_defense_structure(
                c, obs, _TURRET, 75, 0,
                gx, gy,
                worker_type, main_type, pending_workers)
            if p is not None:
                pending[f"build:{_TURRET}"] = p
                pending_workers.add(p.worker_id)
                print(f"[t_v6_6] FIRE  build:Missile_Turret "
                      f"@line_gap=({gx},{gy}) "
                      f"(history={len(defense_line_history)})")

    # (3) v6_4: spread tanks across an arc, not a single point.
    # Each completed tank gets a stable slot index in `tank_slot`.
    # Assignment: fill the lowest free slot index first.
    # Sieged tanks are already committed to a slot and stay put
    # (unsiege happens in the step-advance block up in run()).
    # Tank-mode tanks not at their slot walk toward it.
    all_tanks = [u for u in units
                 if u.get("completed")
                 and u["type"] in (_TANK_MODE, _SIEGE_MODE)]
    # Drop dead tanks from tank_slot.
    live_ids = {t["unit_id"] for t in all_tanks}
    for tid in list(tank_slot.keys()):
        if tid not in live_ids:
            tank_slot.pop(tid, None)
    # Assign slots to any completed tank without one yet.
    used_slots = set(tank_slot.values())
    for t in all_tanks:
        if t["unit_id"] in tank_slot:
            continue
        # Pick the lowest free slot index.
        for s in range(TANK_ARC_SLOTS):
            if s not in used_slots:
                tank_slot[t["unit_id"]] = s
                used_slots.add(s)
                break
        # If all TANK_ARC_SLOTS are taken (we built more than
        # TANK_ARC_SLOTS tanks), extras loop and share slots --
        # they'll just sit close to each other.
        else:
            tank_slot[t["unit_id"]] = t["unit_id"] % TANK_ARC_SLOTS

    # Move Tank_Mode tanks to their assigned slot position.
    for tank in units:
        if not tank.get("completed"):
            continue
        if tank["type"] != _TANK_MODE:
            continue  # sieged tanks are pinned; siege pass unsieges them
        slot = tank_slot.get(tank["unit_id"])
        if slot is None:
            continue
        sx, sy = tank_slot_position(anchor_x, anchor_y,
                                    home_x, home_y, slot)
        d = dist_pixels(tank["x"], tank["y"], sx, sy)
        if d < TANK_SLOT_TOLERANCE_PX:
            continue  # already at slot; siege pass will handle sieging
        order_str = order_name(tank["order"])
        if order_str in ("Move", "AttackMove", "AttackUnit"):
            continue
        try:
            await c.move(unit_id=tank["unit_id"], x=sx, y=sy)
        except Exception as e:
            print(f"[t_v6_6]  tank-arc move error: {e}")

    # (4) Marines: patrol a ring near the nearest bunker.
    # v6_5: reverted from v6_4's `load` fire. The server does ship
    # the `load` verb, but the observation payload doesn't expose
    # whether a Marine is already inside a Bunker
    # (status_flag_loaded / status_flag_in_bunker aren't
    # serialised, and there's no transport_id field). Without
    # observable "who is in the bunker" state we can't dedup load
    # fires and end up spamming `load` every tick on the same
    # Marine. Ring pattern gives adequate defensive coverage in
    # the meantime; see
    # issues/2026-07-14-observation-missing-bunker-load-state.md.
    bunkers = [u for u in units if u["type"] == _BUNKER
               and u.get("completed") is True]
    if bunkers:
        for m in units:
            if m.get("type") != _MARINE or not m.get("completed"):
                continue
            if m["order"] not in IDLE_ORDERS:
                # only re-order when the marine is truly idle --
                # don't yank one out of an attack
                continue
            b = min(bunkers, key=lambda u: dist_pixels(
                u["x"], u["y"], m["x"], m["y"]))
            if dist_pixels(m["x"], m["y"],
                           b["x"], b["y"]) < MARINE_BUNKER_RING_PX:
                continue
            # Random point on the bunker's ring, seeded by
            # (frame + marine_id) so distinct marines pick
            # distinct spots on the ring and it's still
            # deterministic across replays.
            angle = ((frame + m["unit_id"] * 37) % 360) * math.pi / 180.0
            mx = int(b["x"] + math.cos(angle) * MARINE_BUNKER_RING_PX)
            my = int(b["y"] + math.sin(angle) * MARINE_BUNKER_RING_PX)
            try:
                await c.move(unit_id=m["unit_id"], x=mx, y=my)
            except Exception as e:
                print(f"[t_v6_6]  marine-ring move error: {e}")


async def phase_base_garrison(c: Client, obs: dict,
                              home_x: int, home_y: int,
                              worker_type: int, main_type: int,
                              pending_workers: set[int],
                              pending: dict,
                              reserve,
                              base_garrison: dict[int, dict],
                              tank_slot: dict[int, int]) -> None:
    """v6_4: for each own CC that ISN'T the main base, keep 1
    Bunker + 1 Missile Turret near it, dispatch 2 Marines
    (they'll get loaded into the bunker by phase_defense_line's
    load pass), and reserve 1 Tank for that base.

    "Main base" = the CC closest to (home_x, home_y). Everything
    else is an expansion.

    Reserved tanks are pulled out of the front-line arc: their
    unit_id is added to `base_garrison[cc_id]["tank"]` and the
    tank_slot mapping is deleted so phase_defense_line's arc
    pass doesn't try to move them anymore.
    """
    units = obs["units"]
    r = obs["resources"]
    _BUNKER = UNIT_TYPES_BY_NAME["Terran_Bunker"]
    _TURRET = UNIT_TYPES_BY_NAME["Terran_Missile_Turret"]
    _MARINE = UNIT_TYPES_BY_NAME["Terran_Marine"]
    _TANK_MODE = UNIT_TYPES_BY_NAME["Terran_Siege_Tank_Tank_Mode"]
    _SIEGE_MODE = UNIT_TYPES_BY_NAME["Terran_Siege_Tank_Siege_Mode"]

    all_ccs = [u for u in units if u["type"] == main_type
               and u.get("completed") is True
               and not u.get("flying")]
    if len(all_ccs) < 2:
        return  # only main base, no expansion to garrison

    # Identify the main base = CC closest to spawn home. Everything
    # else is an expansion.
    main_cc = min(all_ccs, key=lambda u: dist_pixels(
        u["x"], u["y"], home_x, home_y))
    expansions = [cc for cc in all_ccs if cc["unit_id"] != main_cc["unit_id"]]

    # Prune garrison entries for expansions that no longer exist.
    live_cc_ids = {cc["unit_id"] for cc in expansions}
    for cc_id in list(base_garrison.keys()):
        if cc_id not in live_cc_ids:
            base_garrison.pop(cc_id, None)

    live_unit_ids = {u["unit_id"] for u in units}
    # Also prune dead assignments within each entry.
    for cc_id, g in base_garrison.items():
        if g.get("bunker", -1) not in (-1,) and g["bunker"] not in live_unit_ids:
            g["bunker"] = -1
        if g.get("turret", -1) not in (-1,) and g["turret"] not in live_unit_ids:
            g["turret"] = -1
        g["marines"] = [m for m in g.get("marines", []) if m in live_unit_ids]
        if g.get("tank", -1) not in (-1,) and g["tank"] not in live_unit_ids:
            g["tank"] = -1

    for cc in expansions:
        cc_id = cc["unit_id"]
        g = base_garrison.setdefault(cc_id, {
            "bunker": -1, "turret": -1, "marines": [], "tank": -1})
        cx, cy = cc["x"], cc["y"]

        # (a) Ensure a Bunker within GARRISON_STRUCTURE_PX. Look
        # up existing Bunkers first; adopt one if unclaimed.
        if g["bunker"] == -1:
            near_bunkers = [b for b in units
                            if b["type"] == _BUNKER
                            and b.get("completed")
                            and dist_pixels(b["x"], b["y"], cx, cy) < GARRISON_STRUCTURE_PX]
            claimed = {v["bunker"] for v in base_garrison.values()
                       if v.get("bunker", -1) != -1}
            near_bunkers = [b for b in near_bunkers if b["unit_id"] not in claimed]
            if near_bunkers:
                g["bunker"] = near_bunkers[0]["unit_id"]
                print(f"[t_v6_6]  GARRISON cc{cc_id}: adopted "
                      f"bunker {g['bunker']}")
            elif (f"garrison_bunker:{cc_id}" not in pending
                  and reserve(100, 0)):
                # Fire a new bunker at the CC.
                p = await try_build_defense_structure(
                    c, obs, _BUNKER, 100, 0, cx, cy,
                    worker_type, main_type, pending_workers)
                if p is not None:
                    pending[f"garrison_bunker:{cc_id}"] = p
                    pending_workers.add(p.worker_id)
                    print(f"[t_v6_6] FIRE  build:Bunker "
                          f"@garrison cc{cc_id}=({cx},{cy})")

        # (b) Ensure a Missile Turret.
        if g["turret"] == -1:
            near_turrets = [t for t in units
                            if t["type"] == _TURRET
                            and t.get("completed")
                            and dist_pixels(t["x"], t["y"], cx, cy) < GARRISON_STRUCTURE_PX]
            claimed = {v["turret"] for v in base_garrison.values()
                       if v.get("turret", -1) != -1}
            near_turrets = [t for t in near_turrets if t["unit_id"] not in claimed]
            if near_turrets:
                g["turret"] = near_turrets[0]["unit_id"]
                print(f"[t_v6_6]  GARRISON cc{cc_id}: adopted "
                      f"turret {g['turret']}")
            elif (f"garrison_turret:{cc_id}" not in pending
                  and reserve(75, 0)):
                p = await try_build_defense_structure(
                    c, obs, _TURRET, 75, 0,
                    cx + 80, cy + 80,  # small offset so it doesn't
                                       # try the exact same tile as
                                       # the bunker
                    worker_type, main_type, pending_workers)
                if p is not None:
                    pending[f"garrison_turret:{cc_id}"] = p
                    pending_workers.add(p.worker_id)
                    print(f"[t_v6_6] FIRE  build:Missile_Turret "
                          f"@garrison cc{cc_id}=({cx + 80},{cy + 80})")

        # (c) Reserve up to 2 Marines. Prefer marines that are
        # already close to this CC (idle patrol). Pull them out
        # of the front-line pool by directly moving to the CC
        # area; phase_defense_line's load pass will load them
        # into g["bunker"] once they're near.
        if len(g["marines"]) < 2:
            claimed = set()
            for v in base_garrison.values():
                claimed.update(v.get("marines", []))
            avail_marines = [m for m in units
                             if m["type"] == _MARINE
                             and m.get("completed")
                             and not m.get("transport_id")
                             and m["unit_id"] not in claimed]
            # Prefer marines nearest to this CC.
            avail_marines.sort(
                key=lambda m: dist_pixels(m["x"], m["y"], cx, cy))
            need = 2 - len(g["marines"])
            for m in avail_marines[:need]:
                g["marines"].append(m["unit_id"])
                # Send them toward the garrison bunker (or CC if
                # bunker not up yet); load pass fires from there.
                dst_x = cx
                dst_y = cy
                if g["bunker"] != -1:
                    bunker = next((u for u in units if u["unit_id"] == g["bunker"]), None)
                    if bunker is not None:
                        dst_x = bunker["x"]
                        dst_y = bunker["y"]
                try:
                    await c.move(unit_id=m["unit_id"], x=dst_x, y=dst_y)
                    print(f"[t_v6_6]  GARRISON cc{cc_id}: assign "
                          f"marine {m['unit_id']} -> ({dst_x},{dst_y})")
                except Exception as e:
                    print(f"[t_v6_6]  garrison-marine move error: {e}")

        # (d) Reserve 1 Tank. Pull the highest-slot tank out of
        # the front-line arc so its removal is at an edge, not
        # a gap in the middle. Move it to the CC and let the
        # siege pass siege it at-slot (it won't have a
        # tank_slot anymore, so at_slot=False -> only sieges
        # when enemy in range, which is what we want for a
        # reserve garrison tank).
        if g["tank"] == -1:
            claimed = {v["tank"] for v in base_garrison.values()
                       if v.get("tank", -1) != -1}
            avail_tanks = [t for t in units
                           if t["type"] in (_TANK_MODE, _SIEGE_MODE)
                           and t.get("completed")
                           and t["unit_id"] not in claimed
                           and t["unit_id"] in tank_slot]
            if avail_tanks:
                # Pick the highest slot (edge of the arc) so its
                # removal leaves an even arc.
                t = max(avail_tanks,
                        key=lambda tk: tank_slot.get(tk["unit_id"], 0))
                g["tank"] = t["unit_id"]
                # Remove from front-line arc.
                tank_slot.pop(t["unit_id"], None)
                # Move to the CC. If sieged, unsiege first.
                try:
                    if t["type"] == _SIEGE_MODE:
                        await c.unsiege(unit_id=t["unit_id"])
                    await c.move(unit_id=t["unit_id"], x=cx, y=cy)
                    print(f"[t_v6_6]  GARRISON cc{cc_id}: assign "
                          f"tank {t['unit_id']} -> ({cx},{cy})")
                except Exception as e:
                    print(f"[t_v6_6]  garrison-tank move error: {e}")


def defense_line_ready(units: list[dict],
                       anchor_x: int, anchor_y: int) -> bool:
    """True when it's safe to step the defense anchor forward.

    v6_6: quorum-based. Requires >= LINE_READY_QUORUM (0.60 = 60 %)
    of completed Siege Tanks (either mode) to be within
    DEFENSE_LINE_TIGHT_PX of the current anchor. One stuck-behind
    tank no longer blocks the whole line. Stale-siege unsiege
    (task 40) reels the stragglers back in on its own.
    """
    _TANK_MODE = UNIT_TYPES_BY_NAME["Terran_Siege_Tank_Tank_Mode"]
    _SIEGE_MODE = UNIT_TYPES_BY_NAME["Terran_Siege_Tank_Siege_Mode"]
    tanks = [u for u in units
             if u["type"] in (_TANK_MODE, _SIEGE_MODE)
             and u.get("completed")]
    if not tanks:
        return False
    near = sum(1 for t in tanks
               if dist_pixels(t["x"], t["y"],
                              anchor_x, anchor_y) <= DEFENSE_LINE_TIGHT_PX)
    return near / len(tanks) >= LINE_READY_QUORUM


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
        print(f"[t_v6_6] FIRE  {spec.kind}:{spec.label} @{src['unit_id']} "
              f"cost={spec.cost_min}/{spec.cost_gas}")
        return True
    except Exception as e:
        print(f"[t_v6_6]  {spec.kind} error: {e}")
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
              push_after_frames: int,
              opening_marine_target: int) -> None:
    print(f"[t_v6_6] connected slot={c.welcome.slot} "
          f"frame={c.welcome.current_frame}")
    print(f"[t_v6_6] turtle config: "
          f"opening_marine_target={opening_marine_target} "
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
    # v5-inherited: `addon_attempted` was a one-shot set — once we
    # fired PlaceAddon on a Factory, we never retried. But the sim
    # silent-rejects PlaceAddon if a unit is standing in the addon
    # slot at the exact moment of the fire, and stale entries in
    # this set meant no more Machine Shops ever built. v6_5
    # replaces with per-parent {last_attempt_frame, retry_count}
    # so we can time-based retry and eventually lift the parent
    # if the slot stays blocked.
    addon_attempts: dict[int, dict[str, int]] = {}
    # Parents we've already told to lift-relocate for addon room.
    # Prevents re-lifting the same parent every tick while airborne.
    addon_lifted: set[int] = set()
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

    # v6_3 expansion tracking:
    # * pending_expansion_cluster: pending_key -> cluster centroid.
    #   Populated when try_expand fires. Used to know WHICH cluster
    #   a rejected expansion attempt belonged to.
    # * cluster_reject_count: cluster centroid -> consecutive REJECT
    #   count. Bumped when a CC expansion Pending is dropped by
    #   verify_pending without a completion delta.
    # * cluster_blacklist: cluster centroids that have racked up
    #   >= EXPANSION_REJECT_THRESHOLD REJECTs. Permanently excluded
    #   from pick_expansion_site.
    pending_expansion_cluster: dict[str, tuple[int, int]] = {}
    cluster_reject_count: dict[tuple[int, int], int] = {}
    cluster_blacklist: set[tuple[int, int]] = set()

    # v6 defense-line state.
    # `defense_step`: current fraction along the home -> enemy vector
    # where we're building the defensive wall. Starts at
    # DEFENSE_STEP_START and grows by DEFENSE_STEP_DELTA every time
    # `defense_line_ready` returns True with no enemy nearby.
    defense_step: float = DEFENSE_STEP_START
    last_step_advance_frame: int = -1
    # v6_3: every time defense_step advances, append the OLD anchor
    # position here. phase_defense_line uses this to place Missile
    # Turrets along the whole home -> current-anchor path, so
    # detection coverage doesn't lag behind the tank line.
    defense_line_history: list[tuple[int, int]] = []

    # v6_4 tank arc state.
    # `tank_slot[unit_id]` -> slot index (0..N-1) around the anchor.
    # `tank_slot_next_release` -> which parity (0 or 1) unsieges on
    # the NEXT step-advance. Flipped each advance so half the arc
    # always stays sieged as cover for the other half moving forward.
    tank_slot: dict[int, int] = {}
    tank_slot_next_release: int = 0
    # v6_6: frame each tank ENTERED Siege_Mode. Reset when the tank
    # unsieges. Used by the stale-siege pass to unsiege tanks
    # that have been stuck at an old anchor while the arc moved
    # forward.
    tank_siege_start_frame: dict[int, int] = {}

    # v6_4 per-base garrison state.
    # For each own CC other than the main base, assign one Bunker
    # (its unit_id lives here), one Turret (same), up to 2 Marines
    # (their unit_ids), and up to 1 Tank. The values in the inner
    # dict are unit_ids (or -1 sentinel = "wanted, not yet
    # assigned").
    base_garrison: dict[int, dict[str, int | list[int]]] = {}

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
            print(f"[t_v6_6] race={race} home=({home_x},{home_y}) "
                  f"map={map_w}x{map_h}")
            print(f"[t_v6_6] radial wps: {waypoints_by_mode['radial']}")
            print(f"[t_v6_6] zscan wps: {len(waypoints_by_mode['zscan'])} points "
                  f"({waypoints_by_mode['zscan'][:2]}...)")

        verify_pending(pending, obs, stats, grace_frames)

        # v6_3: track expansion REJECTs per-cluster. If the CC
        # Pending vanished from `pending` (verify_pending drops it
        # on timeout) AND the completed count didn't rise, that's a
        # rejected expansion attempt. Bump the counter for the
        # cluster we aimed at; blacklist if it crosses the
        # threshold.
        if race is not None:
            exp_key = f"build:{main_type}"
            if exp_key in pending_expansion_cluster and exp_key not in pending:
                cluster_pt = pending_expansion_cluster.pop(exp_key)
                # Did we actually complete a new CC? If not, count as REJECT.
                nx_now, _ = count_units(units, main_type)
                if nx_now <= len(own_of_type(units, main_type, only_complete=False)) - 1:
                    # A new CC exists that wasn't there before -- TOOK,
                    # not REJECT. (This is a defensive check; typically
                    # verify_pending only drops on TIMEOUT here.)
                    pass
                else:
                    cluster_reject_count[cluster_pt] = (
                        cluster_reject_count.get(cluster_pt, 0) + 1)
                    n = cluster_reject_count[cluster_pt]
                    if n >= EXPANSION_REJECT_THRESHOLD and cluster_pt not in cluster_blacklist:
                        cluster_blacklist.add(cluster_pt)
                        print(f"[t_v6_6]  EXPAND BLACKLIST cluster "
                              f"{cluster_pt}: {n} REJECTs in a row. "
                              f"Won't try again this game.")
                    else:
                        print(f"[t_v6_6]  EXPAND REJECT tracked for "
                              f"cluster {cluster_pt}: {n}/{EXPANSION_REJECT_THRESHOLD}")
                # Also drop the reservation so pick_expansion_site
                # can pick a different cluster next tick.
                pending_expansion_pts.discard(cluster_pt)

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
                    print(f"[t_v6_6] TOOK  upgrade:{spec.label} "
                          f"(level {lvl})")
            else:  # research
                if spec.enum_id in obs_tech:
                    completed_upgrades.add(key)
                    print(f"[t_v6_6] TOOK  research:{spec.label}")

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
            # v6_3: record the OLD anchor into history BEFORE the
            # step update so phase_defense_line can place a Missile
            # Turret there and every prior anchor. The chain of
            # turrets covers the whole home -> current-anchor
            # path against cloaked / invisible attackers.
            defense_line_history.append((anchor_x, anchor_y))
            defense_step = min(DEFENSE_STEP_MAX,
                               defense_step + step_delta)
            last_step_advance_frame = frame
            new_anchor = defense_anchor(
                home_x, home_y, tgt_x, tgt_y,
                defense_step, map_w, map_h)
            mode_str = "PUSH" if push_mode else "def"
            print(f"[t_v6_6]  DEFENSE step advanced to {defense_step:.2f} "
                  f"[{mode_str}] @frame {frame}; new anchor={new_anchor} "
                  f"(tanks_at_anchor={tanks_at_anchor}, "
                  f"line_history={len(defense_line_history)})")
            # v6_4: rotate-forward. On every step-advance, unsiege
            # ONLY tanks whose slot parity matches
            # `tank_slot_next_release`. The other half stays
            # sieged as cover. Next advance flips the parity, so
            # the other half moves. This prevents the whole tank
            # line from unsieging at once (fatal in push mode).
            released = 0
            for tank in units:
                if tank["type"] != _SIEGE_MODE_ID:
                    continue
                if not tank.get("completed"):
                    continue
                slot = tank_slot.get(tank["unit_id"])
                if slot is None:
                    continue
                if slot % 2 != tank_slot_next_release:
                    continue
                try:
                    await c.unsiege(unit_id=tank["unit_id"])
                    released += 1
                except Exception as e:
                    print(f"[t_v6_6]  rotate-unsiege error: {e}")
            if released > 0:
                print(f"[t_v6_6]  ROTATE unsiege parity={tank_slot_next_release}: "
                      f"{released} tanks released to move to new arc")
            tank_slot_next_release = 1 - tank_slot_next_release

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
        print(f"[t_v6_6] f={frame} min={r['minerals']} gas={r['gas']} "
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
                            print(f"[t_v6_6] FIRE  build:Supply_Depot "
                                  f"({pyl_total2 + 1}/{pylon_target}) "
                                  f"anchor={anchor_pt}")
                        else:
                            budget["min"] += 100  # refund
                    except Exception as e:
                        print(f"[t_v6_6]  supply-depot fire error: {e}")
                        budget["min"] += 100
                else:
                    budget["min"] += 100

        # ---- Priority 3: workers.
        # v6_5: fire up to one SCV per completed CC per tick. Prior
        # versions used a single pending key `train:SCV` which
        # capped concurrent SCV training at 1 across ALL CCs; with
        # 2+ bases we were leaving the second CC idle. Now each CC
        # gets its own pending key `train_scv:<cc_id>` so each CC
        # can be training an SCV concurrently.
        # Also log WORKER-STALL if we WANT to train but couldn't
        # (either no minerals, or no CC available).
        if n_workers < worker_target:
            completed_ccs = [m for m in own_of_type(units, main_type)
                             if not m.get("flying")]
            excluded_this_tick: set[int] = set()
            for cc in completed_ccs:
                cc_key = f"train_scv:{cc['unit_id']}"
                if cc_key in pending:
                    excluded_this_tick.add(cc["unit_id"])
                    continue
                if not reserve(worker_train_min, 0):
                    if frame % 500 == 0:
                        print(f"[t_v6_6]  WORKER-STALL: want SCV "
                              f"(n={n_workers}/{worker_target}) but "
                              f"only {r['minerals']} min available")
                    break
                result = await try_train_worker(
                    c, obs, worker_type, main_type, worker_train_min,
                    exclude_cc_ids=excluded_this_tick)
                if result is not None:
                    p, cc_id = result
                    pending[f"train_scv:{cc_id}"] = p
                    excluded_this_tick.add(cc_id)
                    print(f"[t_v6_6] FIRE  train:SCV ({n_workers + 1}/{worker_target}) "
                          f"@cc{cc_id}")
                else:
                    budget["min"] += worker_train_min
                    break

        # v6_2 opening gate (hoisted above Priority 4 so Refinery
        # can be gated on it too): only lift defense structures +
        # non-army catalog buildings + expansion + Refinery once
        # we have a completed Barracks AND at least
        # `opening_marine_target` completed Marines. Until then,
        # minerals go to Barracks + Marines + Supply Depots.
        _MARINE_TYPE = UNIT_TYPES_BY_NAME["Terran_Marine"]
        _BARRACKS_TYPE = UNIT_TYPES_BY_NAME["Terran_Barracks"]
        marines_completed, _ = count_units(units, _MARINE_TYPE)
        barracks_completed, _ = count_units(units, _BARRACKS_TYPE)
        opening_complete = (
            barracks_completed >= 1
            and marines_completed >= opening_marine_target
        )
        # One-shot flip log for visibility.
        if opening_complete and not getattr(run, "_opening_logged", False):
            print(f"[t_v6_6] OPENING COMPLETE @frame {frame}: "
                  f"barracks={barracks_completed} "
                  f"marines={marines_completed}/{opening_marine_target}. "
                  f"Defense + expansion + tech pass now open.")
            run._opening_logged = True  # type: ignore[attr-defined]

        # ---- Priority 4: gas structure.
        # v6_2: hold Refinery until the opening completes -- Marines
        # don't need gas and 100 min is a full Depot or 2/3 of a
        # Barracks.
        gas_bld = UNIT_TYPES_BY_NAME["Terran_Refinery"]
        gas_c, gas_ip = count_units(units, gas_bld)
        if (opening_complete
                and gas_c + gas_ip == 0
                and f"build:{gas_bld}" not in pending):
            if reserve(100, 0):
                p = await try_build(
                    c, obs, BuildingSpec(gas_bld, 100, 0, "geyser"),
                    worker_type, main_type, supply_type, pending_workers,
                    home_x, home_y, map_w, map_h, anchor_strategy_idx)
                if p is not None:
                    pending[f"build:{gas_bld}"] = p
                    pending_workers.add(p.worker_id)
                    print(f"[t_v6_6] FIRE  build:Refinery")
                else:
                    budget["min"] += 100

        # ---- v6 Priority 4.25: defense line.
        # Bunkers + Missile Turrets + tank-to-anchor. Runs BEFORE
        # expansion and catalog buildings so it gets first crack at
        # the mineral budget -- defensive structures are cheaper
        # than a CC and much more valuable early game.
        # In v6_2 this pass is gated on `opening_complete`.
        await phase_defense_line(
            c, obs, anchor_x, anchor_y, home_x, home_y,
            worker_type, main_type, pending_workers, pending,
            reserve, bunker_target, turret_target,
            opening_complete, defense_line_history, tank_slot)

        # v6_4: per-base garrison for expansions beyond main base.
        # Runs after the front-line defense pass so it uses the
        # remaining mineral budget. Skipped if we haven't finished
        # the opening yet.
        if opening_complete:
            await phase_base_garrison(
                c, obs, home_x, home_y,
                worker_type, main_type, pending_workers, pending,
                reserve, base_garrison, tank_slot)

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
        # v6_2 also gates expansion on `opening_complete` -- until
        # the first Marine batch is out, 400 min are better spent
        # on Marines than on a second CC.
        _BUNKER_TYPE = UNIT_TYPES_BY_NAME["Terran_Bunker"]
        bunker_ready, _ = count_units(units, _BUNKER_TYPE)
        turtle_ready = (bunker_ready >= 1)

        # v6_4: resource-driven expansion trigger. If any own CC has
        # <= DEPLETION_MINERAL_COUNT mineral fields within
        # RESOURCE_NEAR_CC_PX of it, that base is nearly out. Bump
        # the effective target so we build one more CC beyond
        # base_target. BW removes a mineral field from the world
        # once mined out, so `neutrals` count near a CC is a
        # ground-truth proxy for "is this base still viable".
        own_ccs = [u for u in units
                   if u["type"] == main_type
                   and u.get("completed")
                   and not u.get("flying")]
        all_neutrals = obs.get("neutrals", [])
        depleted_bases = 0
        for cc in own_ccs:
            near_fields = 0
            for n in all_neutrals:
                if n.get("type") not in (176, 177, 178):
                    continue
                if dist_pixels(n["x"], n["y"],
                               cc["x"], cc["y"]) < RESOURCE_NEAR_CC_PX:
                    near_fields += 1
            if near_fields <= DEPLETION_MINERAL_COUNT:
                depleted_bases += 1
        effective_base_target = max(base_target, nx_c + depleted_bases + 1)
        if depleted_bases > 0 and frame % 500 == 0:
            print(f"[t_v6_6]  DEPLETION detected: {depleted_bases} base(s) "
                  f"near-empty; effective_base_target={effective_base_target}")

        if (nx_c + nx_ip < effective_base_target and nx_key not in pending
                and len(known_resources) >= 4
                and turtle_ready
                and opening_complete):
            if reserve(400, 0):
                result = await try_expand(
                    c, obs, main_type, worker_type,
                    known_resources, pending_expansion_pts,
                    pending_workers,
                    known_enemies=known_enemies,
                    home_x=home_x, home_y=home_y,
                    cluster_blacklist=cluster_blacklist)
                if result is not None:
                    p, cluster_pt = result
                    pending[nx_key] = p
                    pending_workers.add(p.worker_id)
                    # v6_3: remember which cluster this Pending is
                    # for, so if it REJECTs we can bump the
                    # counter for the RIGHT cluster (multiple
                    # expansion attempts can be in flight to
                    # different clusters over the game).
                    pending_expansion_cluster[nx_key] = cluster_pt
                else:
                    budget["min"] += 400  # refund
        elif nx_c + nx_ip < effective_base_target and not turtle_ready:
            if frame % 200 == 0:
                print(f"[t_v6_6]  EXPAND holdoff: "
                      f"bunker_ready={bunker_ready}/1")

        # ---- Priority 5: catalog buildings (1 per tick).
        # v6_1: respect spec.target_count (Barracks/Factory=2 for
        # redundancy under attack; everything else defaults to 1).
        # v6_2: during opening (before opening_complete), only
        # `opening_ok` specs fire. That means the opener spends
        # its minerals on Barracks + Marines + Depots, not on the
        # whole tech tree. Once the first Marine batch is out,
        # this gate lifts and Eng Bay / Factory / etc. build as
        # before.
        # v6_5: compute the Factory anchor hint = point 60 % of the
        # way from home toward the current defense anchor. Factories
        # placed here are outside the tight main-base cluster
        # (avoids blocked addon slots) but still on our side of
        # the map. Falls back to a point toward the front line if
        # defense_step is still small.
        factory_hint = (
            int(home_x + 0.60 * (anchor_x - home_x)),
            int(home_y + 0.60 * (anchor_y - home_y)),
        )

        catalog_build_this_tick = 0
        for spec in catalog_buildings:
            if not opening_complete and not spec.opening_ok:
                continue
            key = f"build:{spec.type_id}"
            if key in pending: continue
            completed, ip = count_units(units, spec.type_id)
            if completed + ip >= spec.target_count: continue
            if catalog_build_this_tick >= 1: break
            if not reserve(spec.cost_min, spec.cost_gas): continue
            # Pass factory_hint only to Factory specs; other specs
            # ignore it (default None).
            hint = factory_hint if spec.anchor == "factory" else None
            p = await try_build(c, obs, spec, worker_type, main_type,
                                supply_type, pending_workers,
                                home_x, home_y, map_w, map_h,
                                anchor_strategy_idx,
                                factory_anchor_hint=hint)
            anchor_strategy_idx += 1
            if p is not None:
                pending[key] = p
                pending_workers.add(p.worker_id)
                print(f"[t_v6_6] FIRE  build:{unit_type_name(spec.type_id)}")
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
        # v6_5: retry-based addon logic.
        # For each (parent_type, addon_type), consider every own
        # completed parent unit. A parent is "done" for this addon
        # if there's an owned addon_type within ADDON_MATCH_PX of
        # it — the sim doesn't back-link addon->parent on the
        # observation, so proximity is the ground-truth signal.
        # Retry cadence: fire once per parent, then wait
        # ADDON_RETRY_FRAMES between retries. After
        # ADDON_MAX_RETRIES failures, mark the parent for lift-
        # relocate (handled in Priority 7.4 lift-to-safety pass).
        # Rate-limit: 1 fire per tick across ALL addons so we
        # don't burn min+gas on every Factory in one tick.
        addon_fired = False
        for parent_type, addon_type, amin, agas, aname in _ADDON_ATTACHMENTS:
            if addon_fired: break
            for parent in own_of_type(units, parent_type):
                if addon_fired: break
                if parent.get("flying"):
                    # Can't build an addon while airborne.
                    continue
                # Skip if this parent already has its addon (any
                # completed OR in-progress addon_type within
                # ADDON_MATCH_PX counts).
                has_addon = False
                for a in units:
                    if a["type"] != addon_type:
                        continue
                    if dist_pixels(a["x"], a["y"],
                                   parent["x"], parent["y"]) < ADDON_MATCH_PX:
                        has_addon = True
                        break
                if has_addon:
                    continue
                # Retry cadence: only fire if no prior attempt OR
                # enough frames have passed since the last one.
                state = addon_attempts.get(parent["unit_id"], {
                    "last_attempt_frame": -10_000, "retry_count": 0})
                if frame - state["last_attempt_frame"] < ADDON_RETRY_FRAMES:
                    continue
                # If we've exhausted retries, don't fire again --
                # lift-relocate will handle it via Priority 7.4.
                if state["retry_count"] >= ADDON_MAX_RETRIES:
                    continue
                if not reserve(amin, agas): continue
                parent_tile_x = parent["x"] // 32
                parent_tile_y = parent["y"] // 32
                try:
                    await c.cmd({"verb": "build",
                                 "unit": parent["unit_id"],
                                 "unit_type": addon_type,
                                 "tile_x": parent_tile_x,
                                 "tile_y": parent_tile_y,
                                 "order": 36})  # Orders::PlaceAddon
                    addon_attempts[parent["unit_id"]] = {
                        "last_attempt_frame": frame,
                        "retry_count": state["retry_count"] + 1,
                    }
                    print(f"[t_v6_6] FIRE  addon:{aname} on "
                          f"{unit_type_name(parent_type)} "
                          f"{parent['unit_id']} "
                          f"(retry {state['retry_count'] + 1}"
                          f"/{ADDON_MAX_RETRIES})")
                    addon_fired = True
                except Exception as e:
                    budget["min"] += amin; budget["gas"] += agas
                    print(f"[t_v6_6]  addon fire error: {e}")

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
                print(f"[t_v6_6] FIRE  train:{unit_type_name(spec.type_id)} "
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

        # v6_5: lift and re-land a Factory whose addon retries are
        # exhausted. After ADDON_MAX_RETRIES failed PlaceAddon
        # fires, the slot east of the Factory is persistently
        # blocked. Sequence:
        #   (a) If ground + retries exhausted + not yet lifted:
        #       fire lift. Add to addon_lifted so we don't re-lift.
        #   (b) If flying + in addon_lifted: request find_placement
        #       for a fresh Factory spot with clear addon slot and
        #       issue `land` there. On successful land the Factory
        #       reappears grounded; the addon pass re-fires with
        #       retry_count=0 (reset below).
        _FACTORY_TYPE_LIFT = UNIT_TYPES_BY_NAME["Terran_Factory"]
        for bld in units:
            if lifted: break
            if bld.get("type") != _FACTORY_TYPE_LIFT: continue
            if not bld.get("completed"): continue

            # (b) already lifted -> try to land
            if bld.get("flying") and bld["unit_id"] in addon_lifted:
                # Pick a landing spot near the factory anchor with
                # clear addon room. Reuse the same corridor / addon
                # filter used at build time.
                factory_hint_pt = (
                    int(home_x + 0.60 * (anchor_x - home_x)),
                    int(home_y + 0.60 * (anchor_y - home_y)),
                )
                # Need an SCV to name in find_placement (server
                # requires a worker_unit param). Pick the nearest
                # idle SCV.
                _SCV_TYPE = UNIT_TYPES_BY_NAME["Terran_SCV"]
                scvs = [u for u in units
                        if u["type"] == _SCV_TYPE
                        and u.get("completed")
                        and u["order"] in IDLE_ORDERS
                        and u["unit_id"] not in pending_workers]
                if not scvs:
                    scvs = [u for u in units
                            if u["type"] == _SCV_TYPE
                            and u.get("completed")]
                if not scvs:
                    continue
                w = scvs[0]
                try:
                    resp = await c.find_placement(
                        unit_type=_FACTORY_TYPE_LIFT,
                        worker_unit=w["unit_id"],
                        center_x=factory_hint_pt[0],
                        center_y=factory_hint_pt[1],
                        radius_tiles=20, max_results=16)
                    spots = resp.get("spots", [])
                except Exception as e:
                    print(f"[t_v6_6]  land find_placement error: {e}")
                    continue
                # Filter for corridor + clear addon slot.
                neutrals_now = obs.get("neutrals", [])
                existing_buildings = [
                    (u["x"], u["y"]) for u in units
                    if u.get("building") and u.get("completed") is not False
                    and u["unit_id"] != bld["unit_id"]  # ignore the lifted factory itself
                ]
                good = []
                for s in spots:
                    sx = s["tile_x"] * 32 + 16
                    sy = s["tile_y"] * 32 + 16
                    if in_mining_corridor(sx, sy, units, neutrals_now, main_type):
                        continue
                    ax_center = (s["tile_x"] + 5) * 32
                    ay_center = (s["tile_y"] + 2) * 32
                    blocked = False
                    for bx, by in existing_buildings:
                        if dist_pixels(ax_center, ay_center, bx, by) < FACTORY_ADDON_CLEAR_PX:
                            blocked = True
                            break
                    if not blocked:
                        good.append(s)
                if not good:
                    # No clear addon slot found this tick; keep
                    # floating and try again next tick.
                    continue
                spot = good[0]
                try:
                    await c.land(unit_id=bld["unit_id"],
                                 unit_type=_FACTORY_TYPE_LIFT,
                                 tile_x=spot["tile_x"],
                                 tile_y=spot["tile_y"])
                    # Land issued; remove from addon_lifted so a
                    # future retry-exhaustion can lift again.
                    addon_lifted.discard(bld["unit_id"])
                    print(f"[t_v6_6] FIRE  land Terran_Factory "
                          f"{bld['unit_id']} @ ({spot['tile_x']},"
                          f"{spot['tile_y']}) for clear addon slot")
                    lifted = True  # rate-limit
                except Exception as e:
                    print(f"[t_v6_6]  land error: {e}")
                continue

            # (a) ground + retries exhausted -> lift
            if bld.get("flying"): continue
            if bld["unit_id"] in addon_lifted: continue
            state = addon_attempts.get(bld["unit_id"])
            if state is None: continue
            if state["retry_count"] < ADDON_MAX_RETRIES: continue
            try:
                await c.lift(unit_id=bld["unit_id"], x=home_x, y=home_y)
                addon_lifted.add(bld["unit_id"])
                # Reset the retry counter so once it lands the
                # addon pass tries again from scratch.
                addon_attempts[bld["unit_id"]] = {
                    "last_attempt_frame": frame,
                    "retry_count": 0,
                }
                print(f"[t_v6_6] FIRE  lift Terran_Factory "
                      f"{bld['unit_id']} for addon relocation "
                      f"(retries exhausted)")
                lifted = True
            except Exception as e:
                print(f"[t_v6_6]  addon-lift error: {e}")

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
                print(f"[t_v6_6] FIRE  lift "
                      f"{unit_type_name(bld['type'])} {bld['unit_id']} "
                      f"hp={hp}/{hp_max} -> retreat to ({home_x},{home_y})")
                lifted = True
            except Exception as e:
                print(f"[t_v6_6]  lift error: {e}")

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
                print(f"[t_v6_6] FIRE  repair "
                      f"scv={scv['unit_id']} -> "
                      f"{unit_type_name(tgt['type'])} {tgt['unit_id']} "
                      f"hp={tgt.get('hp')}/{tgt.get('hp_max')}")
                repair_fires += 1
                idle_scvs.remove(scv)
                busy_scvs.add(scv["unit_id"])
            except Exception as e:
                print(f"[t_v6_6]  repair error: {e}")

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

        # v6_6: maintain tank_siege_start_frame per unit_id.
        # Set when we observe a tank in Siege_Mode without an
        # existing entry (i.e. just sieged). Cleared when the
        # tank is no longer Siege_Mode (in Tank_Mode or dead).
        live_tank_ids = {u["unit_id"] for u in units
                         if u["type"] in (_TANK_MODE, _SIEGE_MODE)}
        for uid in list(tank_siege_start_frame.keys()):
            if uid not in live_tank_ids:
                tank_siege_start_frame.pop(uid, None)
        for u in units:
            if u["type"] == _SIEGE_MODE and u.get("completed"):
                if u["unit_id"] not in tank_siege_start_frame:
                    tank_siege_start_frame[u["unit_id"]] = frame
            elif u["type"] == _TANK_MODE and u.get("completed"):
                # Reset the counter -- tank has already unsieged.
                tank_siege_start_frame.pop(u["unit_id"], None)

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
                # v6_4: tanks siege when at their assigned arc slot
                # (not just the anchor). Each tank has a
                # `tank_slot` entry pointing to an index; compute
                # that slot's position and check distance to it.
                slot = tank_slot.get(tank["unit_id"])
                if slot is not None:
                    sx, sy = tank_slot_position(anchor_x, anchor_y,
                                                home_x, home_y, slot)
                    at_slot = dist_pixels(
                        tank["x"], tank["y"], sx, sy) < TANK_SLOT_TOLERANCE_PX
                else:
                    at_slot = False

                # v6_6: stale-siege unsiege. If a tank has been
                # sieged for too long AND it's off its slot
                # (i.e. the arc moved forward and this tank is
                # blocking), unsiege it so it can walk. This
                # takes priority over the enemy-in-range hold
                # because a stalled tank at an old anchor is
                # useless anyway -- it's neither where the fight
                # will be nor where the line is going.
                is_stale = (
                    t_type == _SIEGE_MODE
                    and not at_slot
                    and (frame - tank_siege_start_frame.get(
                        tank["unit_id"], frame)) >= STALE_SIEGE_FRAMES
                )
                if is_stale:
                    try:
                        await c.unsiege(unit_id=tank["unit_id"])
                        stale_age = frame - tank_siege_start_frame.get(
                            tank["unit_id"], frame)
                        # Distance to slot for the log line.
                        if slot is not None:
                            sx, sy = tank_slot_position(
                                anchor_x, anchor_y, home_x, home_y, slot)
                            slot_dist = int(dist_pixels(
                                tank["x"], tank["y"], sx, sy))
                        else:
                            slot_dist = -1
                        print(f"[t_v6_6] FIRE  stale-unsiege "
                              f"tank={tank['unit_id']} "
                              f"(sieged for {stale_age}f, "
                              f"slot={slot} drift={slot_dist}px)")
                        siege_fires += 1
                        # Optimistically drop the counter so we
                        # don't re-fire next tick before the
                        # transition lands.
                        tank_siege_start_frame.pop(tank["unit_id"], None)
                        continue
                    except Exception as e:
                        print(f"[t_v6_6]  stale-unsiege error: {e}")

                should_siege = (
                    t_type == _TANK_MODE
                    and (ne_dist <= SIEGE_RANGE_PX or at_slot))
                if should_siege:
                    try:
                        await c.siege(unit_id=tank["unit_id"])
                        reason = (f"enemy@{ne_dist}px"
                                  if ne_dist <= SIEGE_RANGE_PX
                                  else f"at-slot{slot}")
                        print(f"[t_v6_6] FIRE  siege tank={tank['unit_id']} "
                              f"({reason})")
                        siege_fires += 1
                    except Exception as e:
                        print(f"[t_v6_6]  siege error: {e}")
                elif (t_type == _SIEGE_MODE
                        and ne_dist >= UNSIEGE_RANGE_PX
                        and not at_slot):
                    # Only unsiege if we're OFF the slot and no
                    # enemy in range -- otherwise arc tanks
                    # ping-pong between modes.
                    try:
                        await c.unsiege(unit_id=tank["unit_id"])
                        print(f"[t_v6_6] FIRE  unsiege tank={tank['unit_id']} "
                              f"(no enemy within {UNSIEGE_RANGE_PX}px, "
                              f"off slot)")
                        siege_fires += 1
                    except Exception as e:
                        print(f"[t_v6_6]  unsiege error: {e}")

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
                    print(f"[t_v6_6] FIRE  place_mine vulture={vid} "
                          f"@({mx},{my}) drop {drop_idx+1}/3")
                    mine_fires += 1
                except Exception as e:
                    print(f"[t_v6_6]  place_mine error: {e}")

        # ---- Priority 8: attack (idle combat -> nearest NEARBY enemy).
        # v6_2: local-only attack. In v6 and earlier, `obs["enemies"]`
        # includes every enemy currently in ANY of our units' vision --
        # since Terran SCV scouts share sight with the army, an enemy
        # near the enemy base shows up here too. `nearest(unit, enemies)`
        # then picked it as the target and marines rushed across the
        # map to their death.
        # Fix: only consider enemies within ATTACK_HOLD_RADIUS_PX of
        # the attacking unit itself. Anything farther is invisible to
        # this pass. Sieged tanks + bunker marines only respond to what
        # walks into their neighbourhood.
        # When `defense_only=False`, the fallback still attack-moves
        # toward known_enemies -- but only after the local check
        # fails, so units still engage things attacking their base.
        for u in combat_units(units):
            if u["order"] not in IDLE_ORDERS: continue
            try:
                enemies = obs.get("enemies", [])
                nearby = [e for e in enemies
                          if dist_pixels(e["x"], e["y"],
                                         u["x"], u["y"])
                          <= ATTACK_HOLD_RADIUS_PX]
                if nearby:
                    t = nearest(u, nearby)
                    if t is not None:
                        await c.attack(unit_id=u["unit_id"],
                                       target_unit=t["unit_id"])
                        continue
                if defense_only:
                    # No enemy within local radius AND defense-only
                    # mode: hold ground. The defense-line pass
                    # already moved tanks / marines into position.
                    continue
                # Non-defense-only fallback (v5 behaviour): attack-
                # move toward known enemy base (or fallback corner).
                if tgt_x is not None:
                    await c.attack(unit_id=u["unit_id"], target_unit=0,
                                   x=tgt_x, y=tgt_y)
            except Exception as e:
                print(f"[t_v6_6]  attack error: {e}")

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
                    print(f"[t_v6_6]  cover move error: {e}")
        if move_done and not stop_done:
            cands = [u for u in units if u["order"] not in IDLE_ORDERS
                     and not u.get("building")]
            if cands:
                try:
                    await c.stop(unit_id=cands[0]["unit_id"])
                    stop_done = True
                except Exception as e:
                    print(f"[t_v6_6]  cover stop error: {e}")

        await asyncio.sleep(interval_sec)


async def main(api_key, host, port, url, interval_sec, worker_target,
               supply_slack, worker_train_min, pylon_target,
               scout_radial, scout_zscan, base_target,
               bunker_target, turret_target, defense_only,
               push_after_frames, opening_marine_target):
    if url:
        client_kwargs = {"api_key": api_key, "url": url}
    else:
        client_kwargs = {"api_key": api_key, "host": host, "port": port}
    async with Client(**client_kwargs) as c:
        await run(c, interval_sec, worker_target, supply_slack,
                  worker_train_min, pylon_target,
                  scout_radial, scout_zscan, base_target,
                  bunker_target, turret_target, defense_only,
                  push_after_frames, opening_marine_target)


def entrypoint() -> None:
    p = argparse.ArgumentParser(
        prog="python3 -m python_agent.agents.t_agent_v6_6",
        description="Terran v6_6: v6_5 + stale-siege unsiege for "
                    "the tank arc (a tank sieged too long off-slot "
                    "unsieges so the arc can advance) + quorum-based "
                    "line-ready gate (60 %% of tanks near the "
                    "anchor is enough, not 100 %%).")
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
    # v6_2: one scout total, zscan mode (whole-map serpentine).
    # v6/v6_1 sent TWO SCVs (radial + zscan) which cost ~15 % of the
    # early mining rate at exactly the moment we need every mineral
    # for Barracks + Marines. Set --scout-radial 1 to restore the
    # v6 opener.
    p.add_argument("--scout-radial", type=int, default=0,
                   help="SCVs doing 8-point radial ring patrol "
                        "(default 0 in v6_2)")
    p.add_argument("--scout-zscan", type=int, default=1,
                   help="SCVs doing Z-shape sweep of the whole map "
                        "(default 1)")
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
    p.add_argument("--opening-marine-target", type=int, default=6,
                   help="v6_2: how many completed Marines to have "
                        "before lifting the opening gate. Until "
                        "this is met, defense structures + tech + "
                        "expansion are all paused so minerals go "
                        "to Barracks + Marines. Default 6.")
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
                         args.push_after_frames,
                         args.opening_marine_target))
    except KeyboardInterrupt:
        print("\n[t_v6_6] stopped")


if __name__ == "__main__":
    entrypoint()
