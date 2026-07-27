#!/usr/bin/env python3
# validate_hd_mapping.py -- audit tools/anim_dump + sprite_viewer's
# HD Mapping output.
#
# Reads:
#   - hd_mapping.json     (produced by sprite_viewer's HD Mapping tab)
#   - arr/images.tbl      (from an openBW SC:R install extraction)
#
# Produces:
#   - stdout report        (anomalies + summary counts)
#   - <out>.json           (bw_id -> sc_r_row lookup, plus rich metadata
#                           per entry: anim_num, unit_name, comment,
#                           whether an anomaly was flagged and why)
#
# The report is what the HD renderer actually needs: bw_id is the row
# in openBW's arr/images.tbl (the numbering used by bwgame.h and
# iscript state), and sc_r_row picks the SC:R images.rel row we saw
# in the mapping tab. The rest is diagnostic.
#
# Usage:
#   tools/validate_hd_mapping.py \
#     --mapping   /path/to/hd_mapping.json \
#     --images-tbl /path/to/arr/images.tbl \
#     --out       tools/hd_mapping_table.json

from __future__ import annotations

import argparse
import json
import sys
from collections import defaultdict
from pathlib import Path


def read_images_tbl(path: Path) -> list[str]:
    """images.tbl format: u16 count, u16 offsets[count], NUL strings."""
    data = path.read_bytes()
    count = int.from_bytes(data[:2], "little")
    out: list[str] = []
    for i in range(count):
        off = int.from_bytes(data[2 + i * 2 : 4 + i * 2], "little")
        if off == 0 or off >= len(data):
            out.append("")
            continue
        end = data.index(b"\x00", off)
        out.append(data[off:end].decode("latin-1", errors="replace"))
    return out


def build_lookup(names: list[str]) -> dict[str, int]:
    """Case-insensitive: name.lower() -> image_id.
    Guards against duplicate names (unusual, but the loop tolerates)."""
    lut: dict[str, int] = {}
    for i, n in enumerate(names):
        if not n:
            continue
        # First occurrence wins -- images.tbl doesn't repeat identifiers
        # in practice, but if it ever does we prefer the earliest.
        lut.setdefault(n.lower(), i)
    return lut


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--mapping", required=True,
                    help="Path to hd_mapping.json from the HD Mapping tab")
    ap.add_argument("--images-tbl", required=True,
                    help="Path to arr/images.tbl extracted from SC:R "
                         "(or slim MPQ). Provides the authoritative "
                         "bw_id <-> name mapping.")
    ap.add_argument("--out", required=True,
                    help="Where to write the bw_id -> sc_r_row table")
    ap.add_argument("--delta-jump-threshold", type=int, default=5,
                    help="Flag entries where sc_r_row - bw_id jumps by "
                         "more than N from the previous confirmed row "
                         "(sorted by sc_r_row). Default 5.")
    args = ap.parse_args()

    mapping_path = Path(args.mapping)
    tbl_path = Path(args.images_tbl)
    out_path = Path(args.out)

    if not mapping_path.exists():
        print(f"error: {mapping_path} not found", file=sys.stderr)
        return 1
    if not tbl_path.exists():
        print(f"error: {tbl_path} not found", file=sys.stderr)
        return 1

    with mapping_path.open() as f:
        doc = json.load(f)
    mapped = doc.get("mapped", [])
    unmapped = doc.get("unmapped", [])

    names = read_images_tbl(tbl_path)
    lut = build_lookup(names)

    # -------- pass 1: resolve bw_id per entry, note case fixes ------
    entries = []
    case_fixes: list[tuple[str, str]] = []  # (as_picked, canonical)
    unknown_names: list[dict] = []

    for m in mapped:
        picked = m.get("unit_name", "")
        sc_r = m.get("sc_r_row")
        anim = int(m.get("anim_num", -1))
        comment = m.get("comment", "")
        bw = lut.get(picked.lower())
        canonical = None
        if bw is not None and picked != names[bw]:
            canonical = names[bw]
            case_fixes.append((picked, canonical))
        entry = {
            "sc_r_row": sc_r,
            "anim_num": anim,
            "unit_name": picked,
            "canonical_name": canonical,   # None if the pick was already canonical
            "bw_id": bw,
            "comment": comment,
            "flags": [],
        }
        if bw is None:
            entry["flags"].append("unknown_name_in_images_tbl")
            unknown_names.append(entry)
        entries.append(entry)

    # -------- pass 2: structural flags -------------------------------
    # (b) anim_num vs sc_r_row: expect delta in {1, 2}, sometimes 3
    for e in entries:
        d = e["sc_r_row"] - e["anim_num"]
        if d not in (1, 2, 3):
            e["flags"].append(f"anim_num_delta={d}")

    # (c) sc_r_row - bw_id must move monotonically when sorted by sc_r_row
    #     with rare small excursions. A big single-row jump smells like
    #     a misclick.
    with_bw = sorted(
        (e for e in entries if e["bw_id"] is not None),
        key=lambda e: e["sc_r_row"],
    )
    for i in range(1, len(with_bw)):
        prev = with_bw[i - 1]
        cur = with_bw[i]
        prev_delta = prev["sc_r_row"] - prev["bw_id"]
        cur_delta = cur["sc_r_row"] - cur["bw_id"]
        if abs(cur_delta - prev_delta) > args.delta_jump_threshold:
            cur["flags"].append(
                f"delta_jump_from_{prev_delta}_to_{cur_delta}"
                f"_(prev_row_{prev['sc_r_row']})"
            )

    # (d) duplicates
    by_sc = defaultdict(list)
    by_bw = defaultdict(list)
    for e in entries:
        by_sc[e["sc_r_row"]].append(e)
        if e["bw_id"] is not None:
            by_bw[e["bw_id"]].append(e)
    for sc, rows in by_sc.items():
        if len(rows) > 1:
            for r in rows:
                r["flags"].append(f"dup_sc_r_row_x{len(rows)}")
    for bw, rows in by_bw.items():
        if len(rows) > 1:
            for r in rows:
                r["flags"].append(f"dup_bw_id_x{len(rows)}")

    # -------- report -----------------------------------------------
    total = len(entries)
    clean = sum(1 for e in entries if not e["flags"])
    print(f"== hd_mapping.json validator ==")
    print(f"  mapped entries        : {total}")
    print(f"  unmapped rows (open)  : {len(unmapped)}")
    print(f"  clean (no flags)      : {clean}")
    print(f"  with flag             : {total - clean}")
    print()

    if case_fixes:
        print(f"-- {len(case_fixes)} case fixes (informational) --")
        for picked, canon in case_fixes[:10]:
            print(f"  {picked!r} -> {canon!r}")
        if len(case_fixes) > 10:
            print(f"  ... {len(case_fixes) - 10} more")
        print()

    def dump_group(title: str, filt) -> None:
        hits = [e for e in entries if filt(e)]
        if not hits:
            return
        print(f"-- {title}  ({len(hits)}) --")
        for e in hits:
            bw = e["bw_id"] if e["bw_id"] is not None else "?"
            print(f"  sc_r={e['sc_r_row']:>4} anim={e['anim_num']:>4} "
                  f"bw={bw:>4}  {e['unit_name']}  "
                  f"[{', '.join(e['flags'])}]")
        print()

    dump_group("unknown names",
               lambda e: "unknown_name_in_images_tbl" in e["flags"])
    dump_group("delta jumps (possible misclicks)",
               lambda e: any(f.startswith("delta_jump") for f in e["flags"]))
    dump_group("unusual anim_num delta",
               lambda e: any(f.startswith("anim_num_delta")
                             for f in e["flags"]))
    dump_group("duplicates",
               lambda e: any(f.startswith("dup_") for f in e["flags"]))

    # -------- delta drift summary ----------------------------------
    if with_bw:
        first, last = with_bw[0], with_bw[-1]
        print("-- delta drift --")
        print(f"  first mapped: sc_r={first['sc_r_row']} "
              f"bw={first['bw_id']} delta="
              f"{first['sc_r_row'] - first['bw_id']}  ({first['unit_name']})")
        print(f"  last mapped : sc_r={last['sc_r_row']} "
              f"bw={last['bw_id']} delta="
              f"{last['sc_r_row'] - last['bw_id']}  ({last['unit_name']})")
        # Bucket transitions.
        transitions = 0
        prev = None
        for e in with_bw:
            d = e["sc_r_row"] - e["bw_id"]
            if prev is not None and d != prev:
                transitions += 1
            prev = d
        print(f"  distinct delta values seen: "
              f"{len({e['sc_r_row'] - e['bw_id'] for e in with_bw})}")
        print(f"  transitions (delta change): {transitions}")
        print()

    # -------- emit runtime table -----------------------------------
    # The renderer only needs bw_id -> {sc_r_row, anim_num}. We also
    # ship the full entry list so debug builds can print the origin
    # for each mapping.
    runtime = {
        "schema": "openbw_hd_mapping_v1",
        "source": {
            "mapping":    str(mapping_path),
            "images_tbl": str(tbl_path),
        },
        "counts": {
            "mapped": total,
            "unmapped": len(unmapped),
            "clean": clean,
        },
        "bw_id_to_sc_r_row": {
            str(e["bw_id"]): {
                "sc_r_row": e["sc_r_row"],
                "anim_num": e["anim_num"],
                "unit_name": e["canonical_name"] or e["unit_name"],
                "comment": e["comment"],
            }
            for e in entries
            if e["bw_id"] is not None and not any(
                f.startswith("dup_") for f in e["flags"]
            )
        },
        "flagged_entries": [
            e for e in entries if e["flags"]
        ],
    }
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("w") as f:
        json.dump(runtime, f, indent=2)
    print(f"wrote {out_path} "
          f"({len(runtime['bw_id_to_sc_r_row'])} bw_id entries, "
          f"{len(runtime['flagged_entries'])} flagged)")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
