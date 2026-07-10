"""Small helpers used by the sample agents.

Each function takes an observation dict (or its parts) and returns
plain lists/dicts, no side effects. Keep this file lean -- attendees
should be able to read every helper in under 30 seconds.

Design note: everything here works with the raw observation shape
returned by the server (see agent_integration.md). No wrapper types.
"""

from __future__ import annotations

import math
from typing import Iterable

from python_agent.enums import (
    WORKER_TYPES, IDLE_ORDERS, UNIT_TYPES_BY_NAME,
)


# ---- Coordinate math ----

def dist_sq(a: dict, b: dict) -> int:
    """Squared Euclidean distance between two unit-shaped dicts."""
    dx = a["x"] - b["x"]
    dy = a["y"] - b["y"]
    return dx * dx + dy * dy


def dist_pixels(ax: int, ay: int, bx: int, by: int) -> float:
    """Plain Euclidean distance in pixels between two (x,y) points."""
    return math.hypot(ax - bx, ay - by)


def pixel_to_tile(px: int, py: int) -> tuple[int, int]:
    """BW uses 32 pixels per tile."""
    return px // 32, py // 32


def tile_to_pixel(tx: int, ty: int) -> tuple[int, int]:
    """Center of the tile."""
    return tx * 32 + 16, ty * 32 + 16


def radial_waypoints(home_x: int, home_y: int,
                     map_w: int, map_h: int,
                     n: int = 8, radius: int | None = None) -> list[tuple[int, int]]:
    """N points evenly-spaced on a circle around home, clipped to the map.

    Used by scouts for "starburst outward from home" patrol. Radius
    defaults to the smaller half-map dimension so the ring roughly
    reaches the map edges. Points closer to the enemy corner are
    equally likely as points behind us -- that's the point of
    coverage-oriented scouting.
    """
    if radius is None:
        radius = min(map_w, map_h) // 2 - 128  # 4-tile margin from edge
    out: list[tuple[int, int]] = []
    for i in range(n):
        angle = (2 * math.pi * i) / n
        x = int(home_x + radius * math.cos(angle))
        y = int(home_y + radius * math.sin(angle))
        # Clip inside the map with a small margin (unit size).
        x = max(64, min(map_w - 64, x))
        y = max(64, min(map_h - 64, y))
        out.append((x, y))
    return out


def zscan_waypoints(map_w: int, map_h: int,
                    sight_pixels: int = 224,
                    margin_pixels: int = 128) -> list[tuple[int, int]]:
    """Serpentine (Z-shape) waypoint list covering the whole map.

    A single scout that visits every point in this list will pass
    within `sight_pixels // 2` of any point on the map. Rows are
    `sight_pixels` apart (default 7 tiles = 224 px, slightly less
    than a probe's 8-tile sight range to give overlap and cover
    diagonal gaps). Direction alternates each row to form a Z /
    boustrophedon path.

    For a 4096x4096 map at 224 px spacing that's ~18 rows and
    ~36 waypoints total.
    """
    xs = [margin_pixels, map_w - margin_pixels]  # left / right ends
    out: list[tuple[int, int]] = []
    y = margin_pixels
    direction = 0  # 0 = left->right, 1 = right->left
    while y <= map_h - margin_pixels:
        # Endpoints of this row in traversal order.
        if direction == 0:
            out.append((xs[0], y))
            out.append((xs[1], y))
        else:
            out.append((xs[1], y))
            out.append((xs[0], y))
        y += sight_pixels
        direction ^= 1
    return out


# ---- Unit lookups ----

def by_type(units: Iterable[dict], type_ids: Iterable[int]) -> list[dict]:
    """Return units whose 'type' field matches any of type_ids."""
    ids = set(type_ids)
    return [u for u in units if u["type"] in ids]


def by_name(units: Iterable[dict], names: Iterable[str]) -> list[dict]:
    """Return units matching any of the given enum names."""
    ids = {UNIT_TYPES_BY_NAME[n] for n in names}
    return by_type(units, ids)


def workers(units: Iterable[dict]) -> list[dict]:
    """All worker units (SCV/Drone/Probe) in the observation."""
    return by_type(units, WORKER_TYPES)


def idle_workers(units: Iterable[dict]) -> list[dict]:
    """Workers doing nothing (Guard / PlayerGuard / Nothing)."""
    return [u for u in workers(units) if u["order"] in IDLE_ORDERS]


def buildings(units: Iterable[dict]) -> list[dict]:
    """Anything marked with the 'building' flag on the wire."""
    return [u for u in units if u.get("building")]


def combat_units(units: Iterable[dict]) -> list[dict]:
    """Non-worker, non-building own units. Suitable for attack orders."""
    return [
        u for u in units
        if not u.get("building")
        and u["type"] not in WORKER_TYPES
    ]


def nearest(from_unit: dict, candidates: Iterable[dict]) -> dict | None:
    """Return the closest of candidates by squared distance, or None."""
    best = None
    best_d = None
    for c in candidates:
        d = dist_sq(from_unit, c)
        if best is None or d < best_d:
            best, best_d = c, d
    return best


# ---- Race / production ----

WORKER_TO_RACE = {
    UNIT_TYPES_BY_NAME["Terran_SCV"]:      "terran",
    UNIT_TYPES_BY_NAME["Zerg_Drone"]:      "zerg",
    UNIT_TYPES_BY_NAME["Protoss_Probe"]:   "protoss",
}


def guess_race(own_units: Iterable[dict]) -> str | None:
    """Infer race from the first recognizable worker or main structure.

    Returns "terran" / "zerg" / "protoss" / None. Useful when the map
    forces a specific race and the agent doesn't know it upfront.
    """
    for u in own_units:
        r = WORKER_TO_RACE.get(u["type"])
        if r is not None:
            return r
    # Fallback: main structures.
    building_race = {
        UNIT_TYPES_BY_NAME["Terran_Command_Center"]:  "terran",
        UNIT_TYPES_BY_NAME["Zerg_Hatchery"]:          "zerg",
        UNIT_TYPES_BY_NAME["Zerg_Lair"]:              "zerg",
        UNIT_TYPES_BY_NAME["Zerg_Hive"]:              "zerg",
        UNIT_TYPES_BY_NAME["Protoss_Nexus"]:          "protoss",
    }
    for u in own_units:
        r = building_race.get(u["type"])
        if r is not None:
            return r
    return None


# ---- Resource / target catalogs ----

MINERAL_FIELD_TYPES = {
    UNIT_TYPES_BY_NAME["Resource_Mineral_Field"],
    UNIT_TYPES_BY_NAME["Resource_Mineral_Field_Type_2"],
    UNIT_TYPES_BY_NAME["Resource_Mineral_Field_Type_3"],
}

VESPENE_GEYSER_TYPES = {
    UNIT_TYPES_BY_NAME["Resource_Vespene_Geyser"],
}

# Race-specific gas structures. Once completed, a worker sent here via
# the gather verb will enter the HarvestGas cycle.
REFINERY_TYPES = {
    UNIT_TYPES_BY_NAME["Terran_Refinery"],
    UNIT_TYPES_BY_NAME["Zerg_Extractor"],
    UNIT_TYPES_BY_NAME["Protoss_Assimilator"],
}

# Race-specific producer buildings that train ground combat units.
# (Zerg has no producer building in the Terran/Protoss sense -- Larva
# morphs directly; kept here for the completeness check the trainer
# does, since Zergling morph is driven from Larva.)
PRODUCER_TYPES = {
    UNIT_TYPES_BY_NAME["Terran_Barracks"],
    UNIT_TYPES_BY_NAME["Protoss_Gateway"],
}


def mineral_fields(neutrals: Iterable[dict]) -> list[dict]:
    return by_type(neutrals, MINERAL_FIELD_TYPES)


def vespene_geysers(neutrals: Iterable[dict]) -> list[dict]:
    return by_type(neutrals, VESPENE_GEYSER_TYPES)


def own_refineries(own_units: Iterable[dict]) -> list[dict]:
    """Completed own gas structures (Refinery/Extractor/Assimilator)."""
    return [u for u in own_units
            if u["type"] in REFINERY_TYPES and u.get("completed") is True]


def own_producers(own_units: Iterable[dict]) -> list[dict]:
    """Completed own producer buildings (Barracks/Gateway)."""
    return [u for u in own_units
            if u["type"] in PRODUCER_TYPES and u.get("completed") is True]
