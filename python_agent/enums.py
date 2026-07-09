"""Load unit-type and order enum tables from agent_reference/*.json.

The C++ server always speaks integer ids on the wire. This module gives
you names for legibility. Import at agent startup:

    from python_agent.enums import unit_type_id, unit_type_name, order_name
    scv_id = unit_type_id("Terran_SCV")   # -> 7
    unit_type_name(64)                    # -> "Protoss_Probe"
    order_name(6)                         # -> "Move"

Lookups are O(1); tables are loaded once at import.
"""

import json
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parent.parent
_REF_DIR = _REPO_ROOT / "agent_reference"


def _load(name: str) -> dict[int, str]:
    """Load one enum file, keying by int (JSON keys are strings)."""
    with (_REF_DIR / name).open() as f:
        raw = json.load(f)
    return {int(k): v for k, v in raw.items()}


UNIT_TYPES: dict[int, str] = _load("unit_types.json")
ORDERS: dict[int, str] = _load("orders.json")

# Reverse lookups (name -> id). Duplicate names shouldn't exist in
# bwenums.h; if they do the last one wins.
UNIT_TYPES_BY_NAME: dict[str, int] = {v: k for k, v in UNIT_TYPES.items()}
ORDERS_BY_NAME: dict[str, int] = {v: k for k, v in ORDERS.items()}


def unit_type_name(id: int) -> str:
    """Integer -> name. Returns 'Unknown_<id>' if not in the table."""
    return UNIT_TYPES.get(id, f"Unknown_{id}")


def unit_type_id(name: str) -> int:
    """Name -> integer. Raises KeyError if unknown."""
    return UNIT_TYPES_BY_NAME[name]


def order_name(id: int) -> str:
    return ORDERS.get(id, f"Unknown_{id}")


def order_id(name: str) -> int:
    return ORDERS_BY_NAME[name]


# Commonly-needed groupings. Feel free to extend in your own agent.
WORKER_TYPES: set[int] = {
    UNIT_TYPES_BY_NAME["Terran_SCV"],
    UNIT_TYPES_BY_NAME["Zerg_Drone"],
    UNIT_TYPES_BY_NAME["Protoss_Probe"],
}

# "Idle" for a completed unit under player control. BW uses Guard /
# PlayerGuard while a unit is standing around waiting for orders. A unit
# doing anything else (Move, AttackUnit, MiningMinerals, ...) is busy.
IDLE_ORDERS: set[int] = {
    ORDERS_BY_NAME["Guard"],
    ORDERS_BY_NAME["PlayerGuard"],
    ORDERS_BY_NAME["Nothing"],
}
