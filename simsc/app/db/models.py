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
    # identity across identity-provider events.
    cognito_sub: Mapped[str] = mapped_column(String(64), unique=True, nullable=False)
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
    races: Mapped[list[str]] = mapped_column(JSON, nullable=False)
    player_aliases: Mapped[list[str]] = mapped_column(JSON, nullable=False)
    pod_name: Mapped[str] = mapped_column(String(63), nullable=False)
    ingress_name: Mapped[str] = mapped_column(String(63), nullable=False)
    # "creating" | "running" | "ended"
    state: Mapped[str] = mapped_column(String(16), default="creating", nullable=False)
    created_at: Mapped[datetime] = mapped_column(
        DateTime(timezone=True), default=_now, nullable=False
    )
    ended_at: Mapped[Optional[datetime]] = mapped_column(DateTime(timezone=True))
