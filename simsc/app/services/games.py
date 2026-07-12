"""Game lifecycle service.

Wraps k8s_client with DB row upsert / delete + WS URL construction.

M3.1 model: request body carries `player_aliases: list[str]`. For each
alias, we look up ALL active keys, and emit one --user-hash per
(alias, hash) pair. The pod ends up accepting any of the player's
current keys — safer than passing a single plaintext key on the CLI
and correct for "creator != player" cases.
"""
from __future__ import annotations

from datetime import datetime, timezone

from fastapi import HTTPException, status
from sqlalchemy import and_, select
from sqlalchemy.orm import Session

from app.core.config import settings
from app.db.models import ApiKey, Game, User
from app.services import k8s_client


def _resolve_players(
    db: Session, player_aliases: list[str]
) -> list[tuple[str, str, int]]:
    """Return list of (alias, sha256hex, slot) — one entry per
    (player, active_key). Slots are assigned by position in the
    player_aliases list.
    """
    entries: list[tuple[str, str, int]] = []
    for slot, alias in enumerate(player_aliases):
        user = db.execute(
            select(User).where(User.alias == alias)
        ).scalar_one_or_none()
        if user is None:
            raise HTTPException(
                status.HTTP_400_BAD_REQUEST,
                f"no such user: alias={alias!r}",
            )
        if not user.is_enabled:
            raise HTTPException(
                status.HTTP_400_BAD_REQUEST,
                f"user disabled: alias={alias!r}",
            )
        keys = db.execute(
            select(ApiKey).where(
                and_(ApiKey.user_id == user.id, ApiKey.revoked_at.is_(None))
            )
        ).scalars().all()
        if not keys:
            raise HTTPException(
                status.HTTP_400_BAD_REQUEST,
                f"user has no active API keys: alias={alias!r} "
                f"(they need to create one first at /simscapp/)",
            )
        for k in keys:
            entries.append((alias, k.key_hash.hex(), slot))
    return entries


def create_game(
    db: Session,
    owner: User,
    map_name: str,
    races: list[str],
    player_aliases: list[str],
) -> Game:
    if len(player_aliases) != len(races):
        raise HTTPException(
            status.HTTP_400_BAD_REQUEST,
            f"player_aliases length ({len(player_aliases)}) "
            f"must equal races length ({len(races)})",
        )
    user_hashes = _resolve_players(db, player_aliases)
    game_id = k8s_client.make_game_id()
    handles = k8s_client.create_game(
        game_id=game_id,
        map_name=map_name,
        races=races,
        user_hashes=user_hashes,
    )
    row = Game(
        id=game_id,
        owner_user_id=owner.id,
        map=map_name,
        races=races,
        player_aliases=player_aliases,
        pod_name=handles.pod_name,
        ingress_name=handles.ingress_name,
        state="creating",
    )
    db.add(row)
    db.commit()
    db.refresh(row)
    return row


def delete_game(db: Session, game: Game) -> None:
    handles = k8s_client.GameHandles(
        game_id=game.id,
        pod_name=game.pod_name,
        service_name=f"{game.id}-svc",
        ingress_name=game.ingress_name,
    )
    k8s_client.delete_game(handles)
    game.state = "ended"
    game.ended_at = datetime.now(timezone.utc)
    db.commit()


def urls(game: Game) -> dict:
    base = f"wss://{settings.games_host}/game/{game.id}"
    return {
        "agent_url": f"{base}/agent",
        "observer_url": f"{base}/observer",
    }
