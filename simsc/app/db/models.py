"""SQLAlchemy 2.0 declarative models.

Two tables for M2:

  users(id, cognito_sub UNIQUE, alias UNIQUE, email, display_name,
        is_admin, is_enabled, created_at)
  api_keys(id, user_id, key_hash UNIQUE, label, created_at, revoked_at)

alembic autogenerate off Base.metadata.
"""
from __future__ import annotations

from datetime import datetime, timezone
from typing import Optional

from sqlalchemy import (
    JSON,
    Boolean,
    DateTime,
    ForeignKey,
    Integer,
    LargeBinary,
    String,
    UniqueConstraint,
)
from sqlalchemy.orm import DeclarativeBase, Mapped, mapped_column, relationship


def _now() -> datetime:
    return datetime.now(timezone.utc)


class Base(DeclarativeBase):
    pass


class User(Base):
    __tablename__ = "users"

    id: Mapped[int] = mapped_column(Integer, primary_key=True)
    # Cognito's stable, immutable subject claim (UUID). Our primary
    # identity across identity-provider events. NULL for bot users
    # created via POST /api/admin/users; a Postgres partial unique
    # index (`uq_users_cognito_sub_not_null`, migration 0004) keeps
    # non-null values unique.
    cognito_sub: Mapped[Optional[str]] = mapped_column(String(64), nullable=True)
    # Human-facing handle. From Cognito's `preferred_username` on first
    # login; never derived from email.
    alias: Mapped[str] = mapped_column(String(64), unique=True, nullable=False)
    email: Mapped[Optional[str]] = mapped_column(String(255))
    display_name: Mapped[Optional[str]] = mapped_column(String(255))
    is_admin: Mapped[bool] = mapped_column(Boolean, default=False, nullable=False)
    is_enabled: Mapped[bool] = mapped_column(Boolean, default=True, nullable=False)
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), default=_now, nullable=False
    )

    api_keys: Mapped[list["ApiKey"]] = relationship(back_populates="user")


class ApiKey(Base):
    __tablename__ = "api_keys"
    __table_args__ = (UniqueConstraint("key_hash", name="uq_api_keys_hash"),)

    id: Mapped[int] = mapped_column(Integer, primary_key=True)
    user_id: Mapped[int] = mapped_column(
        ForeignKey("users.id", ondelete="CASCADE"), nullable=False, index=True
    )
    key_hash: Mapped[bytes] = mapped_column(LargeBinary(32), nullable=False)
    label: Mapped[Optional[str]] = mapped_column(String(64))
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), default=_now, nullable=False
    )
    revoked_at: Mapped[Optional[datetime]] = mapped_column(DateTime(timezone=True))

    user: Mapped[User] = relationship(back_populates="api_keys")

    @property
    def is_active(self) -> bool:
        return self.revoked_at is None


class Game(Base):
    __tablename__ = "games"

    id: Mapped[str] = mapped_column(String(24), primary_key=True)
    owner_user_id: Mapped[int] = mapped_column(
        ForeignKey("users.id", ondelete="CASCADE"), nullable=False, index=True
    )
    map: Mapped[str] = mapped_column(String(255), nullable=False)
    # Per-slot race choice; length matches map's player count.
    races: Mapped[list[str]] = mapped_column(JSON, nullable=False)
    # Per-slot alias. Entries: real alias, "AIBot" (treated as empty
    # for M4), or None (empty). Length matches races.
    player_aliases: Mapped[list[Optional[str]]] = mapped_column(JSON, nullable=False)
    # Sim tick pacing: one of the nine names defined in
    # `services/games.py::GAME_SPEEDS`. Stored as name (not ms) so
    # future re-tunings of the name→ms table don't retro-change
    # finished games. Default "fastest" matches openbw_server's
    # baked-in default (42 ms/frame).
    game_speed: Mapped[str] = mapped_column(
        String(16), default="fastest", nullable=False
    )
    # Pod / ingress names — set once the game transitions to running.
    pod_name: Mapped[Optional[str]] = mapped_column(String(63))
    ingress_name: Mapped[Optional[str]] = mapped_column(String(63))
    # pending_invitations | running | ended | cancelled
    state: Mapped[str] = mapped_column(String(20), default="pending_invitations", nullable=False)
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), default=_now, nullable=False
    )
    started_at: Mapped[Optional[datetime]] = mapped_column(DateTime(timezone=True))
    ended_at: Mapped[Optional[datetime]] = mapped_column(DateTime(timezone=True))


class GameInvitation(Base):
    __tablename__ = "game_invitations"

    game_id: Mapped[str] = mapped_column(
        ForeignKey("games.id", ondelete="CASCADE"), primary_key=True
    )
    alias: Mapped[str] = mapped_column(String(64), primary_key=True)
    # pending | accepted | declined | expired
    status: Mapped[str] = mapped_column(String(16), nullable=False)
    invited_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), default=_now, nullable=False
    )
    responded_at: Mapped[Optional[datetime]] = mapped_column(DateTime(timezone=True))
