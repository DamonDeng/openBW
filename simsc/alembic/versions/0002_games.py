"""games

Revision ID: 0002
Revises: 0001
Create Date: 2026-07-12 20:00:00.000000
"""
from __future__ import annotations

from typing import Sequence, Union

from alembic import op
import sqlalchemy as sa

revision: str = "0002"
down_revision: Union[str, None] = "0001"
branch_labels: Union[str, Sequence[str], None] = None
depends_on: Union[str, Sequence[str], None] = None


def upgrade() -> None:
    op.create_table(
        "games",
        sa.Column("id", sa.String(24), primary_key=True),
        sa.Column(
            "owner_user_id",
            sa.Integer,
            sa.ForeignKey("users.id", ondelete="CASCADE"),
            nullable=False,
            index=True,
        ),
        sa.Column("map", sa.String(255), nullable=False),
        sa.Column("races", sa.JSON, nullable=False),
        sa.Column("player_aliases", sa.JSON, nullable=False),
        sa.Column("pod_name", sa.String(63), nullable=False),
        sa.Column("ingress_name", sa.String(63), nullable=False),
        sa.Column(
            "state",
            sa.String(16),
            nullable=False,
            server_default=sa.text("'creating'"),
        ),
        sa.Column(
            "created_at",
            sa.DateTime(timezone=True),
            nullable=False,
            server_default=sa.text("now()"),
        ),
        sa.Column("ended_at", sa.DateTime(timezone=True)),
    )


def downgrade() -> None:
    op.drop_table("games")
