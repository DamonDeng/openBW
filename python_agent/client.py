"""Thin async client for the openBW agent WebSocket API.

One method per wire message type. Every method returns the parsed JSON
dict as-is; no schema massaging. Higher-level convenience (find
workers, pick nearest mineral, etc.) belongs in agent code, not here.

Usage:

    from python_agent.client import Client
    async with Client(api_key="sk-...") as c:
        obs = await c.observe(targets=["units", "resources"])
        await c.move(unit_id=obs["units"][0]["unit_id"], x=1000, y=1000)

The client raises on:
  - failed HTTP upgrade (401/400) via websockets.exceptions
  - malformed server response (json.JSONDecodeError)
  - server-sent {"type":"error"} for a request whose id matches -- the
    caller should catch AgentError to distinguish from transport errors.
"""

from __future__ import annotations

import asyncio
import json
import uuid
from dataclasses import dataclass
from typing import Any, Iterable

import websockets


class AgentError(RuntimeError):
    """Server replied with type=error to one of our requests."""

    def __init__(self, request_id: str, message: str) -> None:
        super().__init__(f"[{request_id}] {message}")
        self.request_id = request_id
        self.message = message


@dataclass
class Welcome:
    slot: int
    current_frame: int


class Client:
    """Async WebSocket client scoped to one agent connection.

    Not thread-safe. One Client == one active connection == one player slot.
    """

    def __init__(
        self,
        api_key: str,
        host: str = "127.0.0.1",
        port: int = 6113,
        path: str = "/agent",
        url: str | None = None,
        action_log_path: str | None = None,
    ) -> None:
        # Two connection modes:
        #   1. Legacy host+port+path -> ws://host:port/path?key=…
        #      Used by every dev/soak invocation against a local server.
        #   2. Full URL -> passthrough, with ?key=… appended.
        #      Used to connect through the simsc ALB where the URL is
        #      wss://simsc.…/game/<id>/agent (TLS-terminated at the LB).
        # url wins if both are supplied.
        if url is not None:
            sep = '&' if '?' in url else '?'
            self._url = f"{url}{sep}key={api_key}"
        else:
            self._url = f"ws://{host}:{port}{path}?key={api_key}"
        self._ws: Any = None
        self.welcome: Welcome | None = None
        # If two requests are outstanding, responses can interleave.
        # Track pending futures keyed by request id so we don't hand a
        # cmd-ack back to an observe caller.
        self._pending: dict[str, asyncio.Future[dict]] = {}
        self._reader_task: asyncio.Task | None = None
        # Optional action-issue log. When action_log_path is set, every
        # outgoing 'cmd' message writes one line here BEFORE ws.send,
        # so the trace survives even if the client crashes mid-send.
        # Format:
        #   AGENT_ISSUE_CLIENT<TAB>t_mono_ns=<n><TAB>rid=<hex><TAB>slot=<n><TAB>verb=<v><TAB>payload=<json>
        # Join to server-side AGENT_ISSUE via `rid`, then to sim-side
        # AGENT_SCHED_LOCAL / SEND / APPLY via slot+bytes.
        self._action_log = None
        if action_log_path:
            # Line-buffered so a crash still flushes partials.
            self._action_log = open(action_log_path, "a", buffering=1)

    # ---- lifecycle ----
    async def __aenter__(self) -> "Client":
        await self.connect()
        return self

    async def __aexit__(self, *exc_info) -> None:
        await self.close()

    async def connect(self) -> Welcome:
        self._ws = await websockets.connect(self._url)
        msg = json.loads(await self._ws.recv())
        if msg.get("type") != "welcome":
            raise AgentError("<connect>",
                f"expected welcome, got {msg!r}")
        self.welcome = Welcome(slot=msg["slot"], current_frame=msg["current_frame"])
        self._reader_task = asyncio.create_task(self._reader_loop())
        return self.welcome

    async def close(self) -> None:
        if self._reader_task:
            self._reader_task.cancel()
            try:
                await self._reader_task
            except asyncio.CancelledError:
                pass
        if self._ws:
            await self._ws.close()
            self._ws = None
        if self._action_log:
            try:
                self._action_log.close()
            except Exception:
                pass
            self._action_log = None

    # ---- reader loop ----
    async def _reader_loop(self) -> None:
        """Dispatch incoming frames to whoever's awaiting each id."""
        try:
            async for raw in self._ws:
                msg = json.loads(raw)
                rid = msg.get("id")
                fut = self._pending.pop(rid, None) if rid else None
                if fut and not fut.done():
                    if msg.get("type") == "error":
                        fut.set_exception(
                            AgentError(rid, msg.get("message", "unknown")))
                    else:
                        fut.set_result(msg)
                # Untagged / unmatched frames are dropped. The server
                # currently doesn't send any (no push events yet), but
                # future event streams would land here.
        except (websockets.exceptions.ConnectionClosed, asyncio.CancelledError):
            pass
        # Wake up anyone still waiting -- otherwise they hang forever.
        for fut in self._pending.values():
            if not fut.done():
                fut.set_exception(AgentError("<closed>", "connection closed"))
        self._pending.clear()

    async def _request(self, msg: dict) -> dict:
        """Send a JSON message; await the reply with matching id."""
        rid = msg.get("id") or uuid.uuid4().hex
        msg["id"] = rid
        loop = asyncio.get_event_loop()
        fut: asyncio.Future[dict] = loop.create_future()
        self._pending[rid] = fut
        # Action-issue log: capture cmd sends BEFORE ws.send so a
        # client crash still leaves the trace. Skip non-cmd messages
        # (observe / query) — those don't affect sim state.
        if self._action_log and msg.get("type") == "cmd":
            cmd = msg.get("cmd") or {}
            verb = cmd.get("verb", "")
            slot = self.welcome.slot if self.welcome else -1
            # time.monotonic_ns is jitter-free relative to itself,
            # good for ordering events across an agent's own send
            # stream. Wall-clock isn't needed for replay determinism.
            import time as _time
            self._action_log.write(
                f"AGENT_ISSUE_CLIENT\tt_mono_ns={_time.monotonic_ns()}"
                f"\trid={rid}\tslot={slot}\tverb={verb}"
                f"\tpayload={json.dumps(cmd, separators=(',', ':'))}\n"
            )
        await self._ws.send(json.dumps(msg))
        return await fut

    # ---- API surface ----
    async def observe(self, targets: Iterable[str] | None = None,
                      id: str | None = None) -> dict:
        """Snapshot of what our slot can see."""
        payload: dict[str, Any] = {"type": "observe"}
        if id is not None:
            payload["id"] = id
        if targets is not None:
            payload["targets"] = list(targets)
        return await self._request(payload)

    async def find_placement(self, unit_type: int,
                             worker_unit: int | None = None,
                             center_x: int | None = None,
                             center_y: int | None = None,
                             radius_tiles: int = 12,
                             max_results: int = 24,
                             id: str | None = None) -> dict:
        """Ask the server where a building can be placed.

        Returns the placement_result dict:
            {"type": "placement_result", "unit_type": <int>,
             "tile_size_x": <int>, "tile_size_y": <int>,
             "spots": [{"tile_x", "tile_y", "center_x", "center_y"}, ...]}

        spots is ordered nearest-first. Empty spots means nothing
        valid within the radius.
        """
        payload: dict[str, Any] = {
            "type": "find_placement",
            "unit_type": unit_type,
            "radius_tiles": radius_tiles,
            "max_results": max_results,
        }
        if id is not None:
            payload["id"] = id
        if worker_unit is not None:
            payload["worker_unit"] = worker_unit
        if center_x is not None:
            payload["center_x"] = center_x
        if center_y is not None:
            payload["center_y"] = center_y
        return await self._request(payload)

    async def cmd(self, verb_payload: dict, id: str | None = None) -> dict:
        """Send a raw command dict. Prefer the typed helpers below."""
        payload = {"type": "cmd", "cmd": verb_payload}
        if id is not None:
            payload["id"] = id
        return await self._request(payload)

    # Typed helpers for the five supported verbs. Return the ack dict
    # ({"type":"ack","id":..., "queued_at_frame":F}). Raises AgentError
    # if the server rejects the command JSON (unknown verb, missing
    # fields). Note: a successful ack does NOT mean the unit obeyed --
    # invalid targets, dead units, or insufficient resources drop
    # silently inside the sim. Re-observe to confirm effect.
    async def move(self, unit_id: int, x: int, y: int,
                   queue: bool = False) -> dict:
        return await self.cmd({"verb": "move", "unit": unit_id,
                               "x": x, "y": y, "queue": queue})

    async def attack(self, unit_id: int, target_unit: int = 0,
                     x: int = 0, y: int = 0, queue: bool = False) -> dict:
        return await self.cmd({"verb": "attack", "unit": unit_id,
                               "target_unit": target_unit,
                               "x": x, "y": y, "queue": queue})

    async def gather(self, unit_id: int, target_unit: int) -> dict:
        """Send a worker to harvest a mineral field or vespene geyser."""
        return await self.cmd({"verb": "gather", "unit": unit_id,
                               "target_unit": target_unit})

    async def stop(self, unit_id: int, queue: bool = False) -> dict:
        return await self.cmd({"verb": "stop", "unit": unit_id,
                               "queue": queue})

    async def train(self, unit_id: int, unit_type: int) -> dict:
        return await self.cmd({"verb": "train", "unit": unit_id,
                               "unit_type": unit_type})

    async def build(self, unit_id: int, unit_type: int,
                    tile_x: int, tile_y: int,
                    order: int | None = None) -> dict:
        """Place a building at (tile_x, tile_y). The server picks the
        placement order from the target unit_type by default:
          - Terran (106..122)   -> PlaceBuilding (30)
          - Protoss (154..172)  -> PlaceProtossBuilding (31)
          - Zerg (130..150)     -> defaults to PlaceBuilding; caller
            MUST pass order=25 (DroneStartBuild) for Drone -> building.
        `order` overrides the default (used for Zerg builds and Terran
        addons: order=36 = PlaceAddon)."""
        payload = {"verb": "build", "unit": unit_id,
                   "unit_type": unit_type,
                   "tile_x": tile_x, "tile_y": tile_y}
        if order is not None:
            payload["order"] = order
        return await self.cmd(payload)

    async def research(self, unit_id: int, tech: int) -> dict:
        """Research a tech (single-target ability) at a building.
        `tech` is a TechTypes enum int (e.g. Psionic_Storm = 4)."""
        return await self.cmd({"verb": "research", "unit": unit_id,
                               "tech": tech})

    async def upgrade(self, unit_id: int, upgrade: int) -> dict:
        """Start a level-N upgrade at a building. `upgrade` is an
        UpgradeTypes enum int (e.g. Protoss_Ground_Weapons = 1). Sim
        infers the level from current progress -- caller doesn't pass
        it."""
        return await self.cmd({"verb": "upgrade", "unit": unit_id,
                               "upgrade": upgrade})

    async def train_fighter(self, unit_id: int) -> dict:
        """Have a Protoss Carrier or Reaver build one of its baby
        fighter units (Interceptor for Carrier, Scarab for Reaver).
        The parent picks the correct type based on its own type_id.
        Sim rejects silently at capacity."""
        return await self.cmd({"verb": "train_fighter", "unit": unit_id})

    async def load(self, unit_id: int, target_unit: int) -> dict:
        """Order `unit_id` (a passenger — e.g. Marine) to enter
        `target_unit` (a transport or bunker). Sim silent-rejects if
        the target doesn't provide space, the passenger type can't
        enter (SCV cannot enter Bunker; Marine/Firebat/Ghost can),
        or the two units are on different teams. After a successful
        load the passenger's `transport_id` field in the observation
        will point at target_unit."""
        return await self.cmd({"verb": "load", "unit": unit_id,
                               "target_unit": target_unit})

    async def unload(self, unit_id: int, target_unit: int) -> dict:
        """Eject one specific passenger from a transport or bunker.
        `unit_id` is the CONTAINER (whose action queue drives the
        unload); `target_unit` is the passenger being kicked out.
        Note the swapped semantics vs `load`."""
        return await self.cmd({"verb": "unload", "unit": unit_id,
                               "target_unit": target_unit})

    async def unload_all(self, unit_id: int) -> dict:
        """Evacuate every passenger from a transport/bunker at retail
        unload cadence (not instantaneous). Useful when the container
        is about to die."""
        return await self.cmd({"verb": "unload_all", "unit": unit_id})

    async def repair(self, unit_id: int, target_unit: int) -> dict:
        """Terran SCV repair: order the SCV to repair a friendly
        damaged mechanical unit or building. Sim rejects silently on
        non-SCV workers, bio targets, undamaged targets, or non-
        friendly targets. The SCV consumes minerals + gas (matching
        the target's costs, prorated by damage) while repairing."""
        return await self.cmd({"verb": "repair", "unit": unit_id,
                               "target_unit": target_unit})

    async def siege(self, unit_id: int) -> dict:
        """Terran Siege Tank -> Siege Mode. Requires Tank_Siege_Mode
        tech researched at Machine Shop; sim silent-rejects otherwise.
        The tank's unit_type changes from Terran_Siege_Tank_Tank_Mode
        (5) to Terran_Siege_Tank_Siege_Mode (30). It gains long-range
        AoE splash and loses mobility."""
        return await self.cmd({"verb": "siege", "unit": unit_id})

    async def unsiege(self, unit_id: int) -> dict:
        """Terran Siege Tank -> Tank Mode. Mirror of `siege`; drops
        the tank from Siege_Mode back to mobile Tank_Mode."""
        return await self.cmd({"verb": "unsiege", "unit": unit_id})

    async def place_mine(self, unit_id: int, x: int, y: int) -> dict:
        """Terran Vulture drops a Spider Mine at position (x, y) in
        pixels. Requires Spider_Mines tech researched at Machine Shop.
        Each Vulture carries up to 3 mines; sim silent-rejects when the
        Vulture has no mines left or the tech isn't researched."""
        return await self.cmd({"verb": "place_mine", "unit": unit_id,
                               "x": x, "y": y})

    async def lift(self, unit_id: int, x: int, y: int) -> dict:
        """Terran building takes off from the ground and flies to the
        given pixel destination. Only these building types can lift:
        Command_Center (106), Barracks (111), Factory (113),
        Starport (114), Science_Facility (116). Sim silent-rejects
        for other buildings or when the building is still under
        construction. While airborne, the building's `flying` flag
        is set in the observation and it can't train units."""
        return await self.cmd({"verb": "lift", "unit": unit_id,
                               "x": x, "y": y})

    async def land(self, unit_id: int, unit_type: int,
                   tile_x: int, tile_y: int) -> dict:
        """Terran flying building descends to a tile. `unit_type` MUST
        equal the flying building's own type_id (the sim's
        `unit_build_order_valid` check enforces this). Tile must be
        clear -- landing on top of other units silent-rejects.
        Buildings resume being usable (training units, producing)
        once they finish landing."""
        return await self.cmd({"verb": "land", "unit": unit_id,
                               "unit_type": unit_type,
                               "tile_x": tile_x, "tile_y": tile_y})

    async def morph(self, unit_id: int, unit_type: int) -> dict:
        """Zerg unit morph. Source unit is consumed into a Zerg_Egg (or
        Lurker_Egg / Cocoon) which then hatches as `unit_type`.

        Valid source -> target combinations enforced by the sim:
          - Zerg_Larva (35) -> any of Zerg_Drone/Zergling/Overlord/
            Hydralisk/Mutalisk/Scourge/Queen/Defiler/Ultralisk/
            Infested_Terran.
          - Zerg_Hydralisk (38) -> Zerg_Lurker (requires Lurker_Aspect
            tech researched at the Hydralisk Den).
          - Zerg_Mutalisk (43) -> Zerg_Guardian or Zerg_Devourer
            (requires Greater_Spire).
        Sim silent-rejects if the source unit is not one of these, if
        the target isn't a legal morph, or if minerals/gas/supply/tech
        aren't sufficient."""
        return await self.cmd({"verb": "morph", "unit": unit_id,
                               "unit_type": unit_type})

    async def morph_building(self, unit_id: int, unit_type: int) -> dict:
        """Zerg building tier morph. The source building's unit_type
        changes in place (no Egg intermediate for tier morphs).

        Valid source -> target combinations:
          - Zerg_Hatchery (131) -> Zerg_Lair (132) (needs Spawning_Pool)
          - Zerg_Lair (132) -> Zerg_Hive (133) (needs Queens_Nest)
          - Zerg_Spire (141) -> Zerg_Greater_Spire (137) (needs Hive)
          - Zerg_Creep_Colony (143) -> Zerg_Sunken_Colony (146)
            (needs Spawning_Pool) or Zerg_Spore_Colony (144)
            (needs Evolution_Chamber).
        This verb is ONLY for tier morphs on an existing Zerg building
        (action_morph_building at actions.h:888 enforces
        unit_is_zerg_building on the selection). To have a Drone create
        a new Zerg building, use `build(drone_id, building_type,
        tile_x, tile_y, order=25)` instead -- order 25 is
        Orders::DroneStartBuild."""
        return await self.cmd({"verb": "morph_building", "unit": unit_id,
                               "unit_type": unit_type})
