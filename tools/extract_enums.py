#!/usr/bin/env python3
"""Extract UnitTypes and Orders enums from bwenums.h into JSON tables.

Output:
  agent_reference/unit_types.json  { "0": "Terran_Marine", ..., "228": "None" }
  agent_reference/orders.json      { "0": "Die", "1": "Stop", ... }

The tables are keyed by the integer id (as a string, so it round-trips
through JSON cleanly). Value is the enum name from bwenums.h. Some
entries are None or unused placeholders; we keep them so ids line up.

Run: python3 tools/extract_enums.py
"""

import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BWENUMS = ROOT / "bwenums.h"
OUT_DIR = ROOT / "agent_reference"


def parse_enum(text: str, name: str) -> list[str]:
    """Return a list of member names in declaration order, or raise."""
    m = re.search(rf"enum struct {name}\s*:\s*int\s*\{{(.*?)^\}};",
                  text, flags=re.DOTALL | re.MULTILINE)
    if not m:
        raise RuntimeError(f"couldn't find enum {name}")
    body = m.group(1)
    members: list[str] = []
    # Strip // comments and /* */ blocks.
    body = re.sub(r"//[^\n]*", "", body)
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.DOTALL)
    for line in body.splitlines():
        line = line.strip().rstrip(",")
        if not line:
            continue
        # Accept "Name" or "Name = 5". Reject anything with an explicit value
        # that doesn't fit the auto-increment pattern (there are a few in
        # bwenums but they explicitly reset the counter -- we honor them).
        if "=" in line:
            member, _, rest = line.partition("=")
            member = member.strip()
            # Best-effort: try to parse the value if it's a plain int.
            try:
                value = int(rest.strip(), 0)
                # Grow members list to `value` with None padding.
                while len(members) < value:
                    members.append(f"_UNUSED_{len(members)}")
                if len(members) == value:
                    members.append(member)
                else:
                    # Value moves backwards or duplicates; drop.
                    print(f"warning: {name}::{member}={value} conflicts with "
                          f"len={len(members)}; skipping")
            except ValueError:
                # Non-int rhs (e.g. reference to another enum). Skip.
                print(f"warning: skipping {name}::{member} = {rest}")
        else:
            members.append(line)
    return members


def to_id_map(members: list[str]) -> dict[str, str]:
    return {str(i): name for i, name in enumerate(members) if not name.startswith("_UNUSED_")}


def main() -> None:
    text = BWENUMS.read_text()
    OUT_DIR.mkdir(exist_ok=True)

    units = parse_enum(text, "UnitTypes")
    orders = parse_enum(text, "Orders")

    (OUT_DIR / "unit_types.json").write_text(
        json.dumps(to_id_map(units), indent=2) + "\n")
    (OUT_DIR / "orders.json").write_text(
        json.dumps(to_id_map(orders), indent=2) + "\n")

    print(f"wrote {OUT_DIR / 'unit_types.json'} ({len(units)} unit types)")
    print(f"wrote {OUT_DIR / 'orders.json'}     ({len(orders)} orders)")


if __name__ == "__main__":
    main()
