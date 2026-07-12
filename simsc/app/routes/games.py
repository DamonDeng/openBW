"""/api/games and adjacent routes.

Games lifecycle (M4):

  POST   /api/games                 — create (draft → pending or launch)
  GET    /api/games                 — my games + games I'm invited to
  GET    /api/games/{id}            — single-game detail w/ invitations
  POST   /api/games/{id}/accept     — invitee accepts
  POST   /api/games/{id}/decline    — invitee declines (game deleted)
  POST   /api/games/{id}/cancel     — creator cancels (only pending)
  DELETE /api/games/{id}            — teardown running/ended game

Roster:

  GET    /api/users                 — enabled users for the create modal

Maps:

  GET    /api/maps                  — static manifest (44 rows)
"""
from __future__ import annotations

import json
import pathlib
from datetime import datetime
from typing import Optional

from fastapi import APIRouter, Depends, HTTPException, status
from pydantic import BaseModel, Field
from sqlalchemy import or_, select
from sqlalchemy.orm import Session

from app.auth.deps import require_admin, require_user
from app.db.models import Game, GameInvitation, User
from app.db.session import get_db
from app.services import games as games_service
from app.services import k8s_client

router = APIRouter(prefix="/api/games")

_MAPS_PATH = pathlib.Path(__file__).parent.parent / "static" / "maps.json"


# ---- serialization -------------------------------------------------


class InvitationOut(BaseModel):
    alias: str
    status: str
    invited_at: datetime
    responded_at: Optional[datetime]


class GameOut(BaseModel):
    game_id: str
    owner_alias: str
    map: str
    races: list[str]
    player_aliases: list[Optional[str]]
    state: str
    invitations: list[InvitationOut]
    pod_phase: Optional[str] = None
    created_at: datetime
    started_at: Optional[datetime]
    ended_at: Optional[datetime]
    agent_url: Optional[str] = None
    observer_url: Optional[str] = None
    # For SPA convenience: from the caller's perspective, is there an
    # actionable pending invitation on this game?
    my_invitation_status: Optional[str] = None


def _to_out(
    db: Session, game: Game, caller: User, include_pod_phase: bool = False
) -> GameOut:
    owner = db.get(User, game.owner_user_id)
    invs = db.execute(
        select(GameInvitation).where(GameInvitation.game_id == game.id)
    ).scalars().all()
    urls = games_service.urls(game)
    phase = None
    if include_pod_phase and game.pod_name and game.state == "running":
        try:
            phase = k8s_client.pod_phase(game.pod_name)
        except Exception:
            phase = "unknown"
    mine = next((i.status for i in invs if i.alias == caller.alias), None)
    return GameOut(
        game_id=game.id,
        owner_alias=owner.alias if owner else "?",
        map=game.map,
        races=game.races,
        player_aliases=game.player_aliases,
        state=game.state,
        invitations=[
            InvitationOut(
                alias=i.alias, status=i.status,
                invited_at=i.invited_at, responded_at=i.responded_at,
            )
            for i in invs
        ],
        pod_phase=phase,
        created_at=game.created_at,
        started_at=game.started_at,
        ended_at=game.ended_at,
        agent_url=urls.get("agent_url"),
        observer_url=urls.get("observer_url"),
        my_invitation_status=mine,
    )


# ---- endpoints -----------------------------------------------------


class CreateGameIn(BaseModel):
    map: str = Field(..., min_length=1, max_length=255)
    races: list[str] = Field(..., min_length=2, max_length=8)
    # Slot list. Real alias, "AIBot", or None. Length matches races.
    player_aliases: list[Optional[str]] = Field(..., min_length=2, max_length=8)


@router.post("", response_model=GameOut)
def create_game_route(
    body: CreateGameIn,
    user: User = Depends(require_user),
    db: Session = Depends(get_db),
) -> GameOut:
    game = games_service.create(
        db, user, body.map, body.races, body.player_aliases
    )
    return _to_out(db, game, user, include_pod_phase=True)


@router.get("", response_model=list[GameOut])
def list_games(
    user: User = Depends(require_user),
    db: Session = Depends(get_db),
) -> list[GameOut]:
    # Anything I own OR anywhere I'm invited/accepted.
    ids_via_inv = db.execute(
        select(GameInvitation.game_id).where(GameInvitation.alias == user.alias)
    ).scalars().all()
    rows = db.execute(
        select(Game).where(
            or_(Game.owner_user_id == user.id, Game.id.in_(ids_via_inv))
        ).order_by(Game.created_at.desc()).limit(50)
    ).scalars().all()
    return [_to_out(db, g, user) for g in rows]


@router.get("/{game_id}", response_model=GameOut)
def get_game(
    game_id: str,
    user: User = Depends(require_user),
    db: Session = Depends(get_db),
) -> GameOut:
    game = db.get(Game, game_id)
    if game is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "no such game")
    # Access: owner, admin, or an invitee.
    if game.owner_user_id != user.id and not user.is_admin:
        inv = db.execute(
            select(GameInvitation).where(
                GameInvitation.game_id == game.id,
                GameInvitation.alias == user.alias,
            )
        ).first()
        if inv is None:
            raise HTTPException(status.HTTP_403_FORBIDDEN, "not your game")
    return _to_out(db, game, user, include_pod_phase=True)


@router.post("/{game_id}/accept", response_model=GameOut)
def accept_game(
    game_id: str,
    user: User = Depends(require_user),
    db: Session = Depends(get_db),
) -> GameOut:
    game = db.get(Game, game_id)
    if game is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "no such game")
    games_service.accept(db, game, user.alias)
    return _to_out(db, game, user, include_pod_phase=True)


@router.post("/{game_id}/decline")
def decline_game(
    game_id: str,
    user: User = Depends(require_user),
    db: Session = Depends(get_db),
) -> dict:
    game = db.get(Game, game_id)
    if game is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "no such game")
    games_service.decline(db, game, user.alias)
    return {"ok": True, "game_deleted": True}


@router.post("/{game_id}/cancel")
def cancel_game(
    game_id: str,
    user: User = Depends(require_user),
    db: Session = Depends(get_db),
) -> dict:
    game = db.get(Game, game_id)
    if game is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "no such game")
    if game.owner_user_id != user.id and not user.is_admin:
        raise HTTPException(status.HTTP_403_FORBIDDEN, "not your game")
    games_service.cancel(db, game)
    return {"ok": True, "game_deleted": True}


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
    if game.state == "pending_invitations":
        # Same as cancel — no pod exists yet.
        games_service.cancel(db, game)
    else:
        games_service.delete_running_or_ended(db, game)
    return {"ok": True}


# ---- admin ---------------------------------------------------------


admin_router = APIRouter(prefix="/api/admin/games")


@admin_router.get("", response_model=list[GameOut])
def admin_list_all(
    admin: User = Depends(require_admin),
    db: Session = Depends(get_db),
) -> list[GameOut]:
    rows = db.execute(
        select(Game).order_by(Game.created_at.desc()).limit(200)
    ).scalars().all()
    return [_to_out(db, g, admin) for g in rows]


# ---- roster + maps (public to any authenticated user) --------------


users_router = APIRouter(prefix="/api")


class UserRoster(BaseModel):
    alias: str
    display_name: Optional[str]


@users_router.get("/users", response_model=list[UserRoster])
def list_users(
    _caller: User = Depends(require_user),
    db: Session = Depends(get_db),
) -> list[UserRoster]:
    """Enabled-user roster for the create-game slot picker.
    Deliberately minimal — no email, no ids, no is_admin.
    """
    rows = db.execute(
        select(User).where(User.is_enabled.is_(True)).order_by(User.alias)
    ).scalars().all()
    return [UserRoster(alias=u.alias, display_name=u.display_name) for u in rows]


@users_router.get("/maps")
def list_maps(_caller: User = Depends(require_user)) -> list[dict]:
    """Static maps manifest, baked into the image."""
    with open(_MAPS_PATH) as f:
        return json.load(f)
