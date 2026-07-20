"""Race-neutral spatial reasoning for openbw agents.

Every agent in this workspace (t_agent_v6_*, p_agent_v4, z_agent_v5)
has independently reinvented the same set of coordinate helpers:
"where's home?", "which mineral cluster is safe to expand to?",
"where does the defense anchor go?", "is this build spot in the
SCV path?", etc. LocalMap collects all of that in one place so a
Zerg or Protoss agent can `from python_agent.local_map import
LocalMap` and reuse the same logic Terran does.

The module knows nothing about Terran / Protoss / Zerg specifics.
It takes two type ids at construct time:
  * `main_type_id`   -- Terran_Command_Center / Zerg_Hatchery /
                        Protoss_Nexus
  * `gas_extractor_type_id` -- Terran_Refinery / Zerg_Extractor /
                        Protoss_Assimilator
and answers every spatial question from the observation shape the
server already emits (`units`, `enemies`, `neutrals`,
`map_info`).

## Wire dependencies

Requires the observation to include:
  * `units` (targets=["units"]) -- own units for home discovery
    and CC positions
  * `neutrals` (targets=["neutrals"]) -- mineral fields and
    vespene geysers for cluster + corridor logic
  * `enemies` (targets=["enemies"]) -- optional, only used by
    `enemy_direction()` and cluster-side preference. Without it
    the module falls back to `map_opposite_of_home()` as the
    presumed enemy vector.
  * `map_info` (targets=["map_info"]) -- fetched ONCE at
    `.attach(client)` time via `c.observe(targets=["map_info"])`;
    map dimensions don't change after connect.

## Not yet available on the wire (see issue
   2026-07-18-map-api-discovery.md)

  * `start_locations[]` per slot. Currently home is derived from
    the centroid of own units at first observation. Once the
    server emits start_locations, `attach()` can prefer that.
  * Per-tile buildable / walkable mask. Corridor guard is
    line-segment geometry only; without a terrain grid we can't
    detect that a spot lies on unwalkable ground.

## Usage sketch

```python
from python_agent.local_map import LocalMap
from python_agent.enums import UNIT_TYPES_BY_NAME

lm = LocalMap(
    main_type_id=UNIT_TYPES_BY_NAME["Terran_Command_Center"],
    gas_extractor_type_id=UNIT_TYPES_BY_NAME["Terran_Refinery"],
)
await lm.attach(client)                       # one-shot map_info fetch
# each tick:
lm.update(obs)
home = lm.home                                 # (x, y)
target = lm.enemy_direction_point()            # (x, y) far along enemy vector
anchor = lm.defense_anchor(step_frac=0.10)
slot   = lm.grid_slot(k=3, spacing_px=192)
spots  = [s for s in resp["spots"]
          if not lm.in_mining_corridor(s["center_x"], s["center_y"])]
site   = lm.pick_expansion_site(own_side_only=True)
```

`LocalMap` is a plain dataclass-like helper -- no async in the
hot path, no server round trips per tick. Everything below the
class docstring is pure observation math.
"""

from __future__ import annotations

import math
import random
from dataclasses import dataclass, field


# --------------------------------------------------------------------
# Shared spatial primitives.
# --------------------------------------------------------------------


def dist_px(ax: int, ay: int, bx: int, by: int) -> float:
    """Plain Euclidean distance between two pixel points."""
    return math.hypot(ax - bx, ay - by)


def point_segment_dist_px(px: float, py: float,
                          ax: float, ay: float,
                          bx: float, by: float) -> float:
    """Shortest distance in pixels from point (px,py) to line
    segment (ax,ay)-(bx,by). Standard vector projection with
    clamping to the endpoints, used by the corridor guard."""
    dx = bx - ax
    dy = by - ay
    if dx == 0 and dy == 0:
        return dist_px(int(px), int(py), int(ax), int(ay))
    t = ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)
    t = max(0.0, min(1.0, t))
    qx = ax + t * dx
    qy = ay + t * dy
    return dist_px(int(px), int(py), int(qx), int(qy))


# --------------------------------------------------------------------
# Neutral resource types on the wire.
#
# The server emits neutral units in `obs["neutrals"]` with a `type`
# field that maps to `UnitTypes` in bwenums.h. Mineral fields come
# in three visual variants (176/177/178) that behave identically;
# vespene geysers are 188. Race gas extractors (Refinery 116,
# Assimilator ~154+something, Extractor ~150+something) REPLACE
# the geyser once built -- the geyser vanishes from neutrals and
# a same-type building appears in `units`. `in_mining_corridor`
# handles the switch by taking a `gas_extractor_type_id` param.
# --------------------------------------------------------------------

MINERAL_TYPES = frozenset({176, 177, 178})
GEYSER_TYPE = 188


# --------------------------------------------------------------------
# LocalMap.
# --------------------------------------------------------------------


@dataclass
class KnownEnemy:
    """Same shape as t_agent_v6_*'s inline KnownEnemy but scoped
    to local_map so downstream code can import it from one place."""
    unit_id: int
    type_id: int
    x: int
    y: int
    first_seen_frame: int


@dataclass
class LocalMap:
    """Race-agnostic spatial helper.

    Fields left at defaults are populated by `.attach()` (map dims)
    and `.update(obs)` (home, known_enemies, known_resources).
    """
    main_type_id: int
    gas_extractor_type_id: int

    # Populated by attach().
    map_w: int = 0
    map_h: int = 0

    # Populated by update() on first call.
    home_x: int = 0
    home_y: int = 0
    home_ready: bool = False

    # Rolling state accumulated across ticks.
    known_enemies: dict[int, KnownEnemy] = field(default_factory=dict)
    # unit_id -> (type_id, x, y). Includes minerals and geysers.
    # Persists across ticks so a scout that saw a cluster once
    # keeps it in the map even after leaving vision.
    known_resources: dict[int, tuple[int, int, int]] = field(default_factory=dict)

    # Corridor tuning. Default 64 px = 2 tiles each side.
    corridor_half_width_px: int = 64
    # Resources are considered "near this CC" for corridor purposes
    # if within this many pixels. Prevents distant minerals from
    # projecting through the base.
    resource_near_cc_px: int = 900

    # --------------------------------------------------------------
    # Lifecycle.
    # --------------------------------------------------------------

    async def attach(self, client) -> None:
        """One-shot: fetch map dimensions from the server. Call
        this after `Client.connect()` and before the first
        `update()`.

        Uses `obs = await c.observe(targets=["map_info"])` which
        the server has always supported. Map dimensions are
        immutable across a game, so we cache them here."""
        obs = await client.observe(targets=["map_info"])
        mi = obs["map_info"]
        self.map_w = mi["width"]
        self.map_h = mi["height"]

    def update(self, obs: dict) -> None:
        """Refresh per-tick derived state.

        First call also discovers home from the centroid of own
        units. Subsequent calls accumulate `known_enemies` and
        `known_resources` monotonically -- neither drops entries
        when a unit leaves vision (agents that care about
        "still-alive" should filter by their own `obs["enemies"]`).
        """
        units = obs.get("units", [])
        if not self.home_ready and units:
            # Home = centroid of frame-0 own units. Correct for the
            # common case (CC / Hatchery / Nexus + workers spawn in
            # a tight cluster). Once server ships start_locations,
            # prefer that.
            self.home_x = sum(u["x"] for u in units) // len(units)
            self.home_y = sum(u["y"] for u in units) // len(units)
            self.home_ready = True

        frame = obs.get("current_frame", 0)
        for e in obs.get("enemies", []):
            # Only structures are "landmarks" for enemy direction --
            # marines walking around aren't a stable signal.
            if not e.get("building"):
                continue
            uid = e["unit_id"]
            if uid in self.known_enemies:
                continue
            self.known_enemies[uid] = KnownEnemy(
                unit_id=uid, type_id=e["type"],
                x=e["x"], y=e["y"], first_seen_frame=frame,
            )

        for n in obs.get("neutrals", []):
            uid = n["unit_id"]
            if uid in self.known_resources:
                continue
            t = n.get("type")
            if t in MINERAL_TYPES or t == GEYSER_TYPE:
                self.known_resources[uid] = (t, n["x"], n["y"])

    # --------------------------------------------------------------
    # Enemy direction / opposite corner fallback.
    # --------------------------------------------------------------

    def enemy_direction_point(self) -> tuple[int, int]:
        """Return a point representing the direction from home to
        the (best-known) enemy.

        Prefers the closest known enemy building. Falls back to
        the map's opposite corner if no enemy has been scouted
        yet, matching the fallback the existing agents use
        (`tgt_x = map_w - home_x`, `tgt_y = map_h - home_y`).
        """
        if not self.home_ready:
            return (self.map_w // 2, self.map_h // 2)
        if self.known_enemies:
            e = min(
                self.known_enemies.values(),
                key=lambda k: dist_px(self.home_x, self.home_y, k.x, k.y),
            )
            return (e.x, e.y)
        return (self.map_w - self.home_x, self.map_h - self.home_y)

    def home_to_enemy_angle(self) -> float:
        """Angle in radians of the home->enemy vector. Useful for
        arc-slot layouts (`tank_slot_position` etc.). Zero if home
        isn't ready or the vector is degenerate."""
        if not self.home_ready:
            return 0.0
        ex, ey = self.enemy_direction_point()
        fx = ex - self.home_x
        fy = ey - self.home_y
        if math.hypot(fx, fy) < 1.0:
            return 0.0
        return math.atan2(fy, fx)

    # --------------------------------------------------------------
    # Defense anchor along the home->enemy vector.
    # --------------------------------------------------------------

    def defense_anchor(self, step_frac: float) -> tuple[int, int]:
        """Point at `step_frac` (0..1) along home -> enemy vector.

        step_frac=0.0 sits on home. 1.0 sits at the enemy point.
        Turtle agents push the anchor forward from 0.10 -> 0.85 by
        bumping this fraction; the value the caller passes is
        opaque to LocalMap.
        """
        if not self.home_ready:
            return (self.map_w // 2, self.map_h // 2)
        ex, ey = self.enemy_direction_point()
        ax = int(self.home_x + step_frac * (ex - self.home_x))
        ay = int(self.home_y + step_frac * (ey - self.home_y))
        return (ax, ay)

    # --------------------------------------------------------------
    # Grid slot spiral (compact base layout).
    # --------------------------------------------------------------

    def grid_slot(self, k: int,
                  spacing_px: int = 192) -> tuple[int, int]:
        """Return (x, y) for the k-th slot on a spiral around home.

        Ring 1 = 8 slots at radius spacing_px, one per 45-degree
        segment; ring 2 = 8 slots at 2*spacing_px offset 22.5 deg
        so they interleave into ring 1's gaps; ring 3 = 8 slots at
        3*spacing_px back on the ring-1 angles; etc.

        The forward direction (slot 0) points toward map center
        from home, so a corner base spreads INWARD rather than
        wasting slots on off-map tiles.
        """
        if not self.home_ready:
            return (0, 0)
        # Forward direction = home -> map center.
        cx = self.map_w // 2
        cy = self.map_h // 2
        fx = cx - self.home_x
        fy = cy - self.home_y
        if math.hypot(fx, fy) < 1.0:
            forward = 0.0
        else:
            forward = math.atan2(fy, fx)
        # 8 slots per ring. Ring index is 1-based so k=0 lands in
        # ring 1 (no slot exactly on the CC).
        ring = k // 8 + 1
        idx_in_ring = k % 8
        base_offset = math.pi / 4 * idx_in_ring
        # Even rings get a half-step offset to interleave.
        ring_offset = (math.pi / 8) if (ring % 2 == 0) else 0.0
        angle = forward + base_offset + ring_offset
        r = ring * spacing_px
        ax = int(self.home_x + math.cos(angle) * r)
        ay = int(self.home_y + math.sin(angle) * r)
        return (ax, ay)

    # --------------------------------------------------------------
    # Arc slot (defense-line tank formation etc.).
    # --------------------------------------------------------------

    def arc_slot(self, k: int, total: int,
                 center_x: int, center_y: int,
                 radius_px: int,
                 arc_span_rad: float = math.pi) -> tuple[int, int]:
        """Return (x, y) for slot k of `total` on an arc.

        The arc spans `arc_span_rad` radians (default pi = 180 deg,
        a semicircle) centered on the home->center direction so
        the arc faces AWAY from home (toward the enemy vector,
        assuming center is on the defense anchor line).

        k=0 is one end of the arc; k=total-1 is the other end.
        """
        if total <= 1:
            return (center_x, center_y)
        fx = center_x - self.home_x
        fy = center_y - self.home_y
        if math.hypot(fx, fy) < 1.0:
            forward = 0.0
        else:
            forward = math.atan2(fy, fx)
        # Slots span [-span/2, +span/2] relative to forward.
        rel = -arc_span_rad / 2 + arc_span_rad * (k / (total - 1))
        angle = forward + rel
        sx = int(center_x + math.cos(angle) * radius_px)
        sy = int(center_y + math.sin(angle) * radius_px)
        return (sx, sy)

    # --------------------------------------------------------------
    # Mining corridor guard.
    # --------------------------------------------------------------

    def in_mining_corridor(self, spot_px_x: int, spot_px_y: int,
                           units: list[dict], neutrals: list[dict]
                           ) -> bool:
        """True if the placement spot sits inside the worker path
        corridor between any own CC/Hatchery/Nexus and any nearby
        mineral field / geyser / same-race gas extractor.

        Called on every candidate spot returned by find_placement
        to reject spots that would block worker walk-out. Cheap
        (a few dist checks per call).
        """
        ccs = [u for u in units if u["type"] == self.main_type_id]
        if not ccs:
            return False
        for cc in ccs:
            cx, cy = cc["x"], cc["y"]
            for r in neutrals:
                rt = r.get("type")
                if rt not in MINERAL_TYPES and rt != GEYSER_TYPE:
                    continue
                rx, ry = r["x"], r["y"]
                if dist_px(cx, cy, rx, ry) > self.resource_near_cc_px:
                    continue
                d = point_segment_dist_px(
                    spot_px_x, spot_px_y, cx, cy, rx, ry)
                if d < self.corridor_half_width_px:
                    return True
            # Once the extractor is built, the geyser vanishes from
            # neutrals -- treat the extractor as an endpoint too.
            for u in units:
                if u["type"] != self.gas_extractor_type_id:
                    continue
                if dist_px(cx, cy, u["x"], u["y"]) > self.resource_near_cc_px:
                    continue
                d = point_segment_dist_px(
                    spot_px_x, spot_px_y, cx, cy, u["x"], u["y"])
                if d < self.corridor_half_width_px:
                    return True
        return False

    # --------------------------------------------------------------
    # Resource clusters + expansion selection.
    # --------------------------------------------------------------

    # Merge threshold: two neutrals within this distance count as
    # the same cluster. Matches CLUSTER_MERGE_PX in v6_9. BW
    # mineral patches sit within ~200 px of each other; 400 gives
    # comfortable merge without joining across a choke.
    cluster_merge_px: int = 400
    # An expansion site must be at least this far from any existing
    # CC (own or pending). Matches MIN_EXPANSION_DIST_PX.
    min_expansion_dist_px: int = 1000

    def clusters(self) -> list[tuple[int, int, int, int]]:
        """Group `known_resources` into (centroid_x, centroid_y,
        mineral_count, geyser_count) tuples. Sorted by mineral
        count desc so the biggest cluster comes first.

        Naive O(n^2) union-find over the tens of neutrals in a
        game.
        """
        pts = list(self.known_resources.values())
        if not pts:
            return []
        parent = list(range(len(pts)))

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
                if dist_px(pts[i][1], pts[i][2],
                           pts[j][1], pts[j][2]) < self.cluster_merge_px:
                    union(i, j)

        groups: dict[int, list[tuple[int, int, int]]] = {}
        for i, p in enumerate(pts):
            groups.setdefault(find(i), []).append(p)

        out = []
        for members in groups.values():
            if not members:
                continue
            xs = [m[1] for m in members]
            ys = [m[2] for m in members]
            cx = sum(xs) // len(xs)
            cy = sum(ys) // len(ys)
            n_min = sum(1 for m in members if m[0] in MINERAL_TYPES)
            n_gas = sum(1 for m in members if m[0] == GEYSER_TYPE)
            out.append((cx, cy, n_min, n_gas))
        out.sort(key=lambda c: c[2], reverse=True)
        return out

    def pick_expansion_site(self, own_ccs: list[dict],
                            pending_expansion_pts: set[tuple[int, int]] | None = None,
                            cluster_blacklist: set[tuple[int, int]] | None = None,
                            own_side_only: bool = True,
                            min_field_count: int = 4,
                            ) -> tuple[int, int] | None:
        """Return centroid of a viable cluster to expand to, or
        None if none is viable.

        - Skips clusters within `min_expansion_dist_px` of any
          own CC (via `own_ccs` param) or pending expansion.
        - Skips clusters blacklisted for consecutive REJECTs.
        - Skips clusters with fewer than `min_field_count`
          mineral fields (small side patches).
        - When `own_side_only=True` and any enemy has been seen,
          only clusters with dist_home < dist_enemy are valid.
          When `own_side_only=False`, all clusters are considered.
        """
        if not own_ccs or not self.known_resources:
            return None
        pending_expansion_pts = pending_expansion_pts or set()
        cluster_blacklist = cluster_blacklist or set()
        clusters = self.clusters()

        # Enemy point for the side filter.
        enemy_pt: tuple[int, int] | None = None
        if self.known_enemies:
            e = min(self.known_enemies.values(),
                    key=lambda k: dist_px(self.home_x, self.home_y, k.x, k.y))
            enemy_pt = (e.x, e.y)

        our_side: list[tuple[int, int, int, int]] = []
        rest: list[tuple[int, int, int, int]] = []
        for c in clusters:
            cx, cy, _, _ = c
            if enemy_pt is not None:
                d_home = dist_px(cx, cy, self.home_x, self.home_y)
                d_enemy = dist_px(cx, cy, enemy_pt[0], enemy_pt[1])
                if d_home < d_enemy:
                    our_side.append(c)
                else:
                    rest.append(c)
            else:
                our_side.append(c)

        def _first_valid(cands):
            for cx, cy, n_min, _n_gas in cands:
                if n_min < min_field_count:
                    continue
                # Blacklist proximity.
                too_close = False
                for bx, by in cluster_blacklist:
                    if dist_px(cx, cy, bx, by) < self.min_expansion_dist_px:
                        too_close = True
                        break
                if too_close:
                    continue
                # Own CC proximity.
                for cc in own_ccs:
                    if dist_px(cx, cy, cc["x"], cc["y"]) < self.min_expansion_dist_px:
                        too_close = True
                        break
                if too_close:
                    continue
                # Pending expansion proximity.
                for px, py in pending_expansion_pts:
                    if dist_px(cx, cy, px, py) < self.min_expansion_dist_px:
                        too_close = True
                        break
                if too_close:
                    continue
                return (cx, cy)
            return None

        candidate = _first_valid(our_side)
        if candidate is not None or own_side_only:
            return candidate
        return _first_valid(rest)

    # --------------------------------------------------------------
    # Convenience: "how far along home -> enemy is this point?"
    # --------------------------------------------------------------

    def project_along_enemy_vector(self, x: int, y: int) -> float:
        """Return the fractional projection of (x, y) onto the
        home->enemy vector. 0.0 means (x, y) is at home; 1.0 at
        the enemy point; >1.0 past it. Negative means behind
        home. Useful for "is this unit ahead of the defense
        line?" checks."""
        if not self.home_ready:
            return 0.0
        ex, ey = self.enemy_direction_point()
        vx = ex - self.home_x
        vy = ey - self.home_y
        mag_sq = vx * vx + vy * vy
        if mag_sq < 1.0:
            return 0.0
        dx = x - self.home_x
        dy = y - self.home_y
        return (dx * vx + dy * vy) / mag_sq

    # --------------------------------------------------------------
    # Public accessors matching what agents used to compute inline.
    # --------------------------------------------------------------

    @property
    def home(self) -> tuple[int, int]:
        return (self.home_x, self.home_y)

    @property
    def map_dims(self) -> tuple[int, int]:
        return (self.map_w, self.map_h)
