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
    ) -> None:
        self._url = f"ws://{host}:{port}{path}?key={api_key}"
        self._ws: Any = None
        self.welcome: Welcome | None = None
        # If two requests are outstanding, responses can interleave.
        # Track pending futures keyed by request id so we don't hand a
        # cmd-ack back to an observe caller.
        self._pending: dict[str, asyncio.Future[dict]] = {}
        self._reader_task: asyncio.Task | None = None

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
                    tile_x: int, tile_y: int) -> dict:
        return await self.cmd({"verb": "build", "unit": unit_id,
                               "unit_type": unit_type,
                               "tile_x": tile_x, "tile_y": tile_y})

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
