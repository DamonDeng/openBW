"""Game lifecycle service.

Wraps k8s_client with DB row upsert / delete + WS URL construction.
"""
from __future__ import annotations

from datetime import datetime, timezone

from sqlalchemy.orm import Session

from app.core.config import settings
from app.db.models import Game, User
from app.services import k8s_client


def create_game(
    db: Session,
    owner: User,
    owner_plain_key: str,
    map_name: str,
    races: list[str],
) -> Game:
    """Owner's plain-text API key is passed to the pod as their `--user`
    slot-0 credential. Other slots run without --user (they can accept
    late-joining humans via the browser SPA once we build the join flow,
    or agents can be added later via POST /api/games/{id}/players).

    For M3 smoke: owner is the only player (slot 0).
    """
    game_id = k8s_client.make_game_id()
    handles = k8s_client.create_game(
        game_id=game_id,
        map_name=map_name,
        races=races,
        users=[(owner.alias, owner_plain_key, 0)],
    )
    row = Game(
        id=game_id,
        owner_user_id=owner.id,
        map=map_name,
        races=races,
        player_aliases=[owner.alias],
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
