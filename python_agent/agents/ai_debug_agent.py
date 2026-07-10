"""Minimal debug agent for isolating server-observer sim divergence.

The v4 agent does too much (scouts, mines, builds, trains, upgrades) —
too many independent action streams to bisect the divergence source.
This agent takes exactly ONE action per invocation (or none), then
observes forever. Combined with `--sync-log` on server + observer, this
lets us diff INVENTORY row-by-row (by st.current_frame + slot) and pin
down which specific agent verb triggers the drift.

The agent supports the following knobs, all optional:

  --observe-only          Never issue any command. Just observe every
                          second. Baseline: does divergence appear with
                          zero agent activity? If YES, the bug is in
                          the sim / catchup, not in any verb.

  --mine-only             Send each idle probe to a mineral field once
                          at start, then observe forever. Isolates
                          whether gather actions cause the drift.

  --one-pylon             Once minerals >= 100, build ONE Pylon with a
                          fixed tile position. Then observe forever.
                          Isolates whether build actions cause drift.

  --one-train             Once minerals >= 50 and a Pylon exists, train
                          ONE Zealot. Then observe forever.

  --grow-econ             Steady-state economy loop:
                            * assign any idle probe to nearest mineral
                            * train Probe from Nexus when idle + can afford
                            * build Pylon whenever supply < supply_max
                          Runs forever. Isolates whether a *stream* of
                          gather/train/build actions (without combat
                          or scouting) reproduces the drift seen in v4.

None-of-the-above = observe-only.

Every action is logged with the server-frame stamp so a diff between
alice's and bob's logs (and between server sync-log and observer
sync-log) pinpoints the exact frame the agent fired.
"""

from __future__ import annotations

import argparse
import asyncio
import sys
import time
from pathlib import Path

# Ensure parent package is importable when run as a script.
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from python_agent.client import Client, AgentError
from python_agent.enums import unit_type_id, unit_type_name


PROBE = unit_type_id("Protoss_Probe")
NEXUS = unit_type_id("Protoss_Nexus")
PYLON = unit_type_id("Protoss_Pylon")
ZEALOT = unit_type_id("Protoss_Zealot")
GATEWAY = unit_type_id("Protoss_Gateway")


def log(msg: str) -> None:
    print(f"[dbg {time.strftime('%H:%M:%S')}] {msg}", flush=True)


def find_units_of_type(units, type_id):
    return [u for u in units if u.get("type") == type_id]


def find_idle_probes(units):
    """All completed probes. In observe/mine-only mode, we don't care
    about ordering nuances — just tell every probe to mine once. The
    server-side gather order will overwrite whatever the probe was
    doing before."""
    return [u for u in units
            if u.get("type") == PROBE and u.get("completed")]


def find_minerals(neutrals):
    return [n for n in neutrals if n.get("type") in (176, 177, 178)]  # Mineral_Field_[1-3]


async def action_mine_all(c, slot_id):
    """Send each idle probe to the nearest mineral. Fires once."""
    obs = await c.observe(targets=["units", "neutrals"])
    probes = find_idle_probes(obs.get("units", []))
    minerals = find_minerals(obs.get("neutrals", []))
    if not minerals:
        log("no minerals visible; skipping mine action")
        return 0
    fired = 0
    for p in probes:
        # nearest mineral
        m = min(minerals, key=lambda mm: abs(mm["x"] - p["x"]) + abs(mm["y"] - p["y"]))
        try:
            await c.gather(unit_id=p["unit_id"], target_unit=m["unit_id"])
            log(f"slot={slot_id} FIRE gather probe={p['unit_id']} -> mineral={m['unit_id']} "
                f"at server_frame={c.welcome.current_frame} (approx)")
            fired += 1
        except AgentError as e:
            log(f"gather error: {e}")
    return fired


async def action_one_pylon(c, slot_id):
    """Build ONE Pylon at a fixed tile relative to home Nexus."""
    obs = await c.observe(targets=["units", "resources"])
    units = obs.get("units", [])
    resources = obs.get("resources", {})
    if resources.get("minerals", 0) < 100:
        return 0
    # already built one?
    pylons = find_units_of_type(units, PYLON)
    if pylons:
        return 0
    # find first idle probe
    probes = [u for u in units if u.get("type") == PROBE and u.get("completed")]
    if not probes:
        return 0
    nexuses = find_units_of_type(units, NEXUS)
    if not nexuses:
        return 0
    nexus = nexuses[0]
    # fixed tile offset from nexus (in tile coords, roughly 4 tiles east)
    tile_x = (nexus["x"] // 32) + 4
    tile_y = (nexus["y"] // 32)
    # find a valid spot via find_placement so we don't fail placement checks
    try:
        pl = await c.find_placement(unit_type=PYLON,
                                    worker_unit=probes[0]["unit_id"],
                                    center_x=nexus["x"],
                                    center_y=nexus["y"],
                                    radius_tiles=8,
                                    max_results=1)
        spots = pl.get("spots", [])
        if not spots:
            log(f"slot={slot_id} no valid pylon spot")
            return 0
        s = spots[0]
        tile_x, tile_y = s["tile_x"], s["tile_y"]
    except AgentError as e:
        log(f"find_placement error: {e}")
        return 0
    try:
        await c.build(unit_id=probes[0]["unit_id"],
                      unit_type=PYLON,
                      tile_x=tile_x, tile_y=tile_y)
        log(f"slot={slot_id} FIRE build Pylon at tile=({tile_x},{tile_y}) "
            f"using probe={probes[0]['unit_id']}")
        return 1
    except AgentError as e:
        log(f"build error: {e}")
        return 0


async def action_grow_econ(c, slot_id, state):
    """Steady-state economy: mine, train probes, build pylons.

    `state` is a mutable dict the caller maintains across calls so we
    don't re-fire pending actions. Keys:
      pending_pylon_probes: set of unit_ids we've already issued build
        Pylon to (avoids double-firing until the next observe reflects
        the new pending building).
      pending_train_frame: server_frame at which we last issued a train
        (avoids re-firing train every 1-sec tick).
    """
    obs = await c.observe(targets=["units", "resources", "neutrals"])
    units = obs.get("units", [])
    resources = obs.get("resources", {})
    neutrals = obs.get("neutrals", [])
    current_frame = obs.get("current_frame", 0)
    minerals_have = resources.get("minerals", 0)
    supply_used = resources.get("supply_used", 0)
    supply_max = resources.get("supply_max", 9)
    fired = 0

    # 1. Send any idle probe to nearest mineral field.
    mineral_fields = find_minerals(neutrals)
    if mineral_fields:
        for p in units:
            if p.get("type") != PROBE: continue
            if not p.get("completed"): continue
            # order 78 is Harvest1 (going to mineral); 80 MoveToMinerals;
            # 81 WaitForMinerals; 82 MiningMinerals; 83 HarvestGather;
            # 84 ReturnMinerals; 85 Harvest2 etc. We treat any order
            # >= 78 and <= 90 as "actively mining" and skip it.
            order = p.get("order", -1)
            if 78 <= order <= 90: continue
            m = min(mineral_fields,
                    key=lambda mm: abs(mm["x"] - p["x"]) + abs(mm["y"] - p["y"]))
            try:
                await c.gather(unit_id=p["unit_id"], target_unit=m["unit_id"])
                fired += 1
                log(f"slot={slot_id} f={current_frame} FIRE gather probe={p['unit_id']}")
            except AgentError as e:
                log(f"gather error: {e}")

    # 2. Train Probe from any idle Nexus, if we can afford it.
    # Probe cost = 50 mins. Reserve enough for other actions.
    if minerals_have >= 50 and supply_used < supply_max:
        for n in units:
            if n.get("type") != NEXUS: continue
            if not n.get("completed"): continue
            # Skip Nexus that's already training something (order 0/1 is
            # Nothing; order 30ish is training). Cheap heuristic: only
            # fire if minerals covered.
            key = f"train:{n['unit_id']}"
            last_frame = state.get(key, -9999)
            if current_frame - last_frame < 30:  # 30 frames = ~1.25 sec
                continue
            try:
                await c.train(unit_id=n["unit_id"], unit_type=PROBE)
                state[key] = current_frame
                minerals_have -= 50
                fired += 1
                log(f"slot={slot_id} f={current_frame} FIRE train Probe from nexus={n['unit_id']}")
                if minerals_have < 50: break
            except AgentError as e:
                log(f"train error: {e}")

    # 3. Build a Pylon if supply is running low and we can afford one.
    # Pylon = 100 mins. Aim for supply headroom.
    supply_deficit = supply_max - supply_used
    if minerals_have >= 100 and supply_deficit <= 3:
        # Not already building?
        pending = state.get("pending_pylon_frame", -9999)
        if current_frame - pending > 60:  # don't spam pylon fires
            probes = [u for u in units
                      if u.get("type") == PROBE and u.get("completed")]
            nexuses = find_units_of_type(units, NEXUS)
            if probes and nexuses:
                nexus = nexuses[0]
                try:
                    pl = await c.find_placement(unit_type=PYLON,
                                                worker_unit=probes[0]["unit_id"],
                                                center_x=nexus["x"],
                                                center_y=nexus["y"],
                                                radius_tiles=10,
                                                max_results=1)
                    spots = pl.get("spots", [])
                    if spots:
                        s = spots[0]
                        await c.build(unit_id=probes[0]["unit_id"],
                                      unit_type=PYLON,
                                      tile_x=s["tile_x"], tile_y=s["tile_y"])
                        state["pending_pylon_frame"] = current_frame
                        fired += 1
                        log(f"slot={slot_id} f={current_frame} FIRE build Pylon "
                            f"at tile=({s['tile_x']},{s['tile_y']}) probe={probes[0]['unit_id']}")
                except AgentError as e:
                    log(f"pylon build error: {e}")

    return fired


async def action_one_train(c, slot_id):
    """Train ONE Zealot from any completed Gateway. Fires once."""
    obs = await c.observe(targets=["units", "resources"])
    units = obs.get("units", [])
    resources = obs.get("resources", {})
    if resources.get("minerals", 0) < 100:
        return 0
    zealots = find_units_of_type(units, ZEALOT)
    if zealots:
        return 0
    gateways = [u for u in units if u.get("type") == GATEWAY and u.get("completed")]
    if not gateways:
        return 0
    try:
        await c.train(unit_id=gateways[0]["unit_id"], unit_type=ZEALOT)
        log(f"slot={slot_id} FIRE train Zealot from gateway={gateways[0]['unit_id']}")
        return 1
    except AgentError as e:
        log(f"train error: {e}")
        return 0


async def run(args):
    async with Client(api_key=args.api_key) as c:
        slot_id = c.welcome.slot
        log(f"connected slot={slot_id} frame={c.welcome.current_frame} "
            f"mode={args.mode}")

        mine_fired = False
        pylon_fired = False
        train_fired = False
        econ_state = {}

        while True:
            obs = await c.observe(targets=["units", "resources"])
            units = obs.get("units", [])
            resources = obs.get("resources", {})
            probes_done = len([u for u in units if u.get("type") == PROBE and u.get("completed")])
            probes_all = len([u for u in units if u.get("type") == PROBE])
            log(f"slot={slot_id} f={obs.get('current_frame')} "
                f"min={resources.get('minerals')} gas={resources.get('gas')} "
                f"probes={probes_done}/{probes_all} "
                f"pyls={len(find_units_of_type(units, PYLON))} "
                f"gate={len([u for u in units if u.get('type') == GATEWAY])}")

            if args.mode == "mine-only" and not mine_fired:
                if await action_mine_all(c, slot_id) > 0:
                    mine_fired = True

            elif args.mode == "one-pylon":
                if not mine_fired:
                    if await action_mine_all(c, slot_id) > 0:
                        mine_fired = True
                elif not pylon_fired:
                    if await action_one_pylon(c, slot_id) > 0:
                        pylon_fired = True

            elif args.mode == "one-train":
                # Train needs Pylon + Gateway to exist; too complex for
                # a minimal repro. Left for later use with a hand-fed
                # setup. Prints a warning if no Gateway ever appears.
                if not train_fired:
                    if await action_one_train(c, slot_id) > 0:
                        train_fired = True

            elif args.mode == "grow-econ":
                await action_grow_econ(c, slot_id, econ_state)

            # else observe-only: no actions.

            await asyncio.sleep(args.interval)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("api_key")
    ap.add_argument("--mode",
                    choices=["observe-only", "mine-only", "one-pylon",
                             "one-train", "grow-econ"],
                    default="observe-only",
                    help="What (minimal) action to take. observe-only "
                         "never fires anything.")
    ap.add_argument("--interval", type=float, default=1.0,
                    help="Seconds between observe calls (default: 1.0)")
    args = ap.parse_args()
    asyncio.run(run(args))


if __name__ == "__main__":
    main()
