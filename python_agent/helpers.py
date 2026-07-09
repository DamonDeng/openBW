"""Small helpers used by the sample agents.

Each function takes an observation dict (or its parts) and returns
plain lists/dicts, no side effects. Keep this file lean -- attendees
should be able to read every helper in under 30 seconds.

Design note: everything here works with the raw observation shape
returned by the server (see agent_integration.md). No wrapper types.
"""

from __future__ import annotations

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


def pixel_to_tile(px: int, py: int) -> tuple[int, int]:
    """BW uses 32 pixels per tile."""
    return px // 32, py // 32


def tile_to_pixel(tx: int, ty: int) -> tuple[int, int]:
    """Center of the tile."""
    return tx * 32 + 16, ty * 32 + 16


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


def mineral_fields(neutrals: Iterable[dict]) -> list[dict]:
    return by_type(neutrals, MINERAL_FIELD_TYPES)


def vespene_geysers(neutrals: Iterable[dict]) -> list[dict]:
    return by_type(neutrals, VESPENE_GEYSER_TYPES)
