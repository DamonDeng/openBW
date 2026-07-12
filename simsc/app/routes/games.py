"""/api/games — game lifecycle endpoints."""
from __future__ import annotations

from datetime import datetime
from typing import Optional

from fastapi import APIRouter, Depends, Header, HTTPException, status
from pydantic import BaseModel, Field
from sqlalchemy import select
from sqlalchemy.orm import Session

from app.auth.deps import require_admin, require_user
from app.db.models import Game, User
from app.db.session import get_db
from app.services import games as games_service
from app.services import k8s_client

router = APIRouter(prefix="/api/games")


class CreateGameIn(BaseModel):
    map: str = Field(..., min_length=1, max_length=255)
    races: list[str] = Field(..., min_length=2, max_length=8)


class GameOut(BaseModel):
    game_id: str
    owner_alias: str
    map: str
    races: list[str]
    state: str
    pod_phase: Optional[str] = None
    created_at: datetime
    ended_at: Optional[datetime]
    agent_url: str
    observer_url: str


def _to_out(db: Session, game: Game, include_pod_phase: bool = False) -> GameOut:
    owner = db.get(User, game.owner_user_id)
    u = games_service.urls(game)
    phase = None
    if include_pod_phase and game.state != "ended":
        try:
            phase = k8s_client.pod_phase(game.pod_name)
        except Exception:
            phase = "unknown"
    return GameOut(
        game_id=game.id,
        owner_alias=owner.alias if owner else "?",
        map=game.map,
        races=game.races,
        state=game.state,
        pod_phase=phase,
        created_at=game.created_at,
        ended_at=game.ended_at,
        agent_url=u["agent_url"],
        observer_url=u["observer_url"],
    )


@router.post("", response_model=GameOut)
def create_game_route(
    body: CreateGameIn,
    x_api_key: str = Header(...),  # noqa: B008 — the plain key comes in here
    user: User = Depends(require_user),
    db: Session = Depends(get_db),
) -> GameOut:
    # Cap games/user to keep the smoke happy — one active game each.
    active = db.execute(
        select(Game).where(
            Game.owner_user_id == user.id, Game.state != "ended"
        )
    ).scalars().all()
    if len(active) >= 3:
        raise HTTPException(
            status.HTTP_400_BAD_REQUEST,
            "max 3 concurrent games per user; DELETE some first",
        )
    game = games_service.create_game(db, user, x_api_key, body.map, body.races)
    return _to_out(db, game, include_pod_phase=True)


@router.get("", response_model=list[GameOut])
def list_my_games(
    user: User = Depends(require_user),
    db: Session = Depends(get_db),
) -> list[GameOut]:
    rows = db.execute(
        select(Game).where(Game.owner_user_id == user.id).order_by(Game.created_at.desc())
    ).scalars().all()
    return [_to_out(db, g) for g in rows]


@router.get("/{game_id}", response_model=GameOut)
def get_game(
    game_id: str,
    user: User = Depends(require_user),
    db: Session = Depends(get_db),
) -> GameOut:
    game = db.get(Game, game_id)
    if game is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "no such game")
    if game.owner_user_id != user.id and not user.is_admin:
        raise HTTPException(status.HTTP_403_FORBIDDEN, "not your game")
    return _to_out(db, game, include_pod_phase=True)


@router.delete("/{game_id}")
def delete_game_route(
    game_id: str,
    user: User = Depends(require_user),
    db: Session = Depends(get_db),
) -> dict:
    game = db.get(Game, game_id)
    if game is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "no such game")
    if game.owner_user_id != user.id and not user.is_admin:
        raise HTTPException(status.HTTP_403_FORBIDDEN, "not your game")
    if game.state == "ended":
        return {"ok": True, "already_ended": True}
    games_service.delete_game(db, game)
    return {"ok": True}


# ---- admin ----


admin_router = APIRouter(prefix="/api/admin/games")


@admin_router.get("", response_model=list[GameOut])
def admin_list_all(
    _admin: User = Depends(require_admin),
    db: Session = Depends(get_db),
) -> list[GameOut]:
    rows = db.execute(select(Game).order_by(Game.created_at.desc())).scalars().all()
    return [_to_out(db, g) for g in rows]
