"""Game lifecycle service (M4).

State machine:

  [create]                       │
      │                          │
      ▼                          │
  pending_invitations ──all-accept──▶ running ──teardown──▶ ended
      │                                                       ▲
      │                                                       │
      ├──creator-cancel─▶ cancelled ──────────────────────────┤
      │                                                       │
      └──any-decline / any-expire ─▶ ("editable" — deleted    │
                                       and re-created by the  │
                                       creator's SPA)         │

Client-side draft model: an "editable" game does NOT live in the DB.
When a decline / expiry drops the game out of pending_invitations, we
DELETE the games row and let the creator's SPA re-open the form
pre-filled with the last inputs. Keeps the server state tiny.
"""
from __future__ import annotations

from datetime import datetime, timedelta, timezone

from fastapi import HTTPException, status
from sqlalchemy import and_, delete, select
from sqlalchemy.orm import Session

from app.core.config import settings
from app.db.models import ApiKey, Game, GameInvitation, User
from app.services import k8s_client

# Slot alias sentinels ------------------------------------------------
AIBOT = "AIBot"
NONE_SLOT = "None"

# 4-hour TTL on pending invitations, per M4 decisions.
INVITATION_TTL = timedelta(hours=4)


def _real_players(player_aliases: list[str | None]) -> list[str]:
    """Return only the real user aliases (drop AIBot/None)."""
    return [
        a for a in player_aliases
        if a is not None and a != AIBOT and a != NONE_SLOT
    ]


def _resolve_hashes(
    db: Session, player_aliases: list[str | None]
) -> list[tuple[str, str, int]]:
    """For each real-user slot, look up all active key hashes and
    emit one (alias, sha256hex, slot) tuple per (alias, key). Empty
    slots (None / AIBot) contribute nothing.
    """
    entries: list[tuple[str, str, int]] = []
    for slot, alias in enumerate(player_aliases):
        if not alias or alias in (AIBOT, NONE_SLOT):
            continue
        user = db.execute(
            select(User).where(User.alias == alias)
        ).scalar_one_or_none()
        if user is None:
            raise HTTPException(
                status.HTTP_400_BAD_REQUEST, f"no such user: {alias!r}"
            )
        if not user.is_enabled:
            raise HTTPException(
                status.HTTP_400_BAD_REQUEST, f"user disabled: {alias!r}"
            )
        keys = db.execute(
            select(ApiKey).where(
                and_(ApiKey.user_id == user.id, ApiKey.revoked_at.is_(None))
            )
        ).scalars().all()
        if not keys:
            raise HTTPException(
                status.HTTP_400_BAD_REQUEST,
                f"user {alias!r} has no active API keys",
            )
        for k in keys:
            entries.append((alias, k.key_hash.hex(), slot))
    return entries


def create(
    db: Session,
    creator: User,
    map_name: str,
    races: list[str],
    player_aliases: list[str | None],
) -> Game:
    """Create a game. If any slot points at a *different* real user,
    the game enters pending_invitations. Otherwise (only creator +
    AIBot / None), the game launches immediately.
    """
    # Slot count matches races.
    if len(player_aliases) != len(races):
        raise HTTPException(
            status.HTTP_400_BAD_REQUEST,
            f"races ({len(races)}) and player_aliases ({len(player_aliases)}) "
            f"length mismatch",
        )
    # End-user rule: they must be a player. Admins are exempt.
    real = _real_players(player_aliases)
    if not creator.is_admin and creator.alias not in real:
        raise HTTPException(
            status.HTTP_400_BAD_REQUEST,
            "end-users must include themselves as a player",
        )
    # Sanity: at least one real player. Otherwise the game is empty.
    if not real:
        raise HTTPException(
            status.HTTP_400_BAD_REQUEST,
            "at least one slot must be a real user",
        )
    # Cap concurrent games per creator (same rule as before).
    active = db.execute(
        select(Game).where(
            Game.owner_user_id == creator.id,
            Game.state.in_(("pending_invitations", "running")),
        )
    ).scalars().all()
    if len(active) >= 3:
        raise HTTPException(
            status.HTTP_400_BAD_REQUEST,
            "you have 3 open games already; end some first",
        )

    invitees = [a for a in real if a != creator.alias]

    game_id = k8s_client.make_game_id()
    game = Game(
        id=game_id,
        owner_user_id=creator.id,
        map=map_name,
        races=races,
        player_aliases=list(player_aliases),
        state="pending_invitations" if invitees else "running",
    )
    db.add(game)
    db.flush()

    now = datetime.now(timezone.utc)
    # Creator is always auto-accepted if they're a player.
    if creator.alias in real:
        db.add(GameInvitation(
            game_id=game.id, alias=creator.alias,
            status="accepted", invited_at=now, responded_at=now,
        ))
    for a in invitees:
        # Verify invitee exists and has at least one active key upfront,
        # so bad invites fail loud at create time rather than at accept.
        u = db.execute(select(User).where(User.alias == a)).scalar_one_or_none()
        if u is None or not u.is_enabled:
            raise HTTPException(
                status.HTTP_400_BAD_REQUEST,
                f"cannot invite {a!r}: no such user or user disabled",
            )
        db.add(GameInvitation(
            game_id=game.id, alias=a, status="pending", invited_at=now,
        ))
    db.commit()
    db.refresh(game)

    if not invitees:
        # No humans to wait on — launch now.
        _launch(db, game)

    return game


def _launch(db: Session, game: Game) -> None:
    """Materialize the k8s Pod + Service + Ingress and mark running."""
    user_hashes = _resolve_hashes(db, game.player_aliases)
    handles = k8s_client.create_game(
        game_id=game.id,
        map_name=game.map,
        races=game.races,
        user_hashes=user_hashes,
    )
    game.pod_name = handles.pod_name
    game.ingress_name = handles.ingress_name
    game.state = "running"
    game.started_at = datetime.now(timezone.utc)
    db.commit()


def accept(db: Session, game: Game, alias: str) -> Game:
    inv = db.execute(
        select(GameInvitation).where(
            GameInvitation.game_id == game.id, GameInvitation.alias == alias
        )
    ).scalar_one_or_none()
    if inv is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "not invited")
    if inv.status != "pending":
        raise HTTPException(
            status.HTTP_400_BAD_REQUEST,
            f"invitation already {inv.status}",
        )
    if game.state != "pending_invitations":
        raise HTTPException(
            status.HTTP_400_BAD_REQUEST,
            f"game is {game.state}, cannot accept",
        )
    inv.status = "accepted"
    inv.responded_at = datetime.now(timezone.utc)
    db.commit()

    # All invitations accepted? Launch.
    pending = db.execute(
        select(GameInvitation).where(
            GameInvitation.game_id == game.id, GameInvitation.status == "pending"
        )
    ).first()
    if pending is None:
        _launch(db, game)
    return game


def decline(db: Session, game: Game, alias: str) -> Game:
    """A decline drops the game back to 'editable' — which in our
    model means: the row is DELETED. The creator's SPA holds the
    form draft client-side and re-opens the modal on the next tick.
    """
    inv = db.execute(
        select(GameInvitation).where(
            GameInvitation.game_id == game.id, GameInvitation.alias == alias
        )
    ).scalar_one_or_none()
    if inv is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "not invited")
    if inv.status != "pending":
        raise HTTPException(
            status.HTTP_400_BAD_REQUEST,
            f"invitation already {inv.status}",
        )
    if game.state != "pending_invitations":
        raise HTTPException(
            status.HTTP_400_BAD_REQUEST,
            f"game is {game.state}, cannot decline",
        )
    _tear_down_pending(db, game, reason="declined")
    return game


def cancel(db: Session, game: Game) -> Game:
    if game.state not in ("pending_invitations",):
        raise HTTPException(
            status.HTTP_400_BAD_REQUEST,
            f"game is {game.state}, cannot cancel here (use DELETE)",
        )
    _tear_down_pending(db, game, reason="cancelled")
    return game


def _tear_down_pending(db: Session, game: Game, reason: str) -> None:
    """Delete the game row + invitations. No pod exists yet at this
    point, so nothing to reach into k8s for. `reason` is unused today
    but reserved for the event log we'll want later.
    """
    db.execute(delete(GameInvitation).where(GameInvitation.game_id == game.id))
    db.delete(game)
    db.commit()


def delete_running_or_ended(db: Session, game: Game) -> None:
    """Teardown for running/ended games — reach into k8s if there's a pod."""
    if game.pod_name:
        handles = k8s_client.GameHandles(
            game_id=game.id,
            pod_name=game.pod_name,
            service_name=f"{game.id}-svc",
            ingress_name=game.ingress_name or f"{game.id}-ing",
        )
        k8s_client.delete_game(handles)
    game.state = "ended"
    game.ended_at = datetime.now(timezone.utc)
    db.commit()


def urls(game: Game) -> dict:
    if not game.pod_name:
        return {}
    base = f"wss://{settings.games_host}/game/{game.id}"
    return {
        "agent_url": f"{base}/agent",
        "observer_url": f"{base}/observer",
    }


def sweep_expired(db: Session) -> int:
    """Called on a timer. Any pending invitation older than the TTL
    is marked expired; any game whose expired-count > 0 gets torn
    down (equivalent to a decline).
    Returns the number of games torn down.
    """
    cutoff = datetime.now(timezone.utc) - INVITATION_TTL
    stale = db.execute(
        select(GameInvitation).where(
            GameInvitation.status == "pending",
            GameInvitation.invited_at < cutoff,
        )
    ).scalars().all()
    if not stale:
        return 0
    now = datetime.now(timezone.utc)
    game_ids: set[str] = set()
    for inv in stale:
        inv.status = "expired"
        inv.responded_at = now
        game_ids.add(inv.game_id)
    db.commit()
    torn = 0
    for gid in game_ids:
        game = db.get(Game, gid)
        if game and game.state == "pending_invitations":
            _tear_down_pending(db, game, reason="expired")
            torn += 1
    return torn
