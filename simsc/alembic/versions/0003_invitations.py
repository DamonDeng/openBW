"""game_invitations + refined state

Revision ID: 0003
Revises: 0002
Create Date: 2026-07-12 21:00:00.000000
"""
from __future__ import annotations

from typing import Sequence, Union

from alembic import op
import sqlalchemy as sa

revision: str = "0003"
down_revision: Union[str, None] = "0002"
branch_labels: Union[str, Sequence[str], None] = None
depends_on: Union[str, Sequence[str], None] = None


def upgrade() -> None:
    # New `state` values: pending_invitations | running | ended | cancelled.
    # The old `creating` is no longer produced; any existing row (only from
    # M3 smoke tests) gets remapped to `ended` since those pods are gone.
    op.execute(
        "UPDATE games SET state = 'ended' WHERE state IN ('creating', 'running')"
    )
    # Widen state column (was 16, we now emit up to len('pending_invitations')=19)
    op.alter_column("games", "state", type_=sa.String(20))
    # M4 lifecycle needs started_at + nullable pod_name/ingress_name (pod
    # is not created at row-insertion time anymore — only after invitees
    # accept).
    op.add_column("games", sa.Column("started_at", sa.DateTime(timezone=True)))
    op.alter_column("games", "pod_name", nullable=True)
    op.alter_column("games", "ingress_name", nullable=True)

    op.create_table(
        "game_invitations",
        sa.Column(
            "game_id",
            sa.String(24),
            sa.ForeignKey("games.id", ondelete="CASCADE"),
            nullable=False,
        ),
        sa.Column("alias", sa.String(64), nullable=False),
        # pending | accepted | declined | expired
        sa.Column("status", sa.String(16), nullable=False),
        sa.Column(
            "invited_at",
            sa.DateTime(timezone=True),
            nullable=False,
            server_default=sa.text("now()"),
        ),
        sa.Column("responded_at", sa.DateTime(timezone=True)),
        sa.PrimaryKeyConstraint("game_id", "alias"),
    )
    op.create_index(
        "ix_game_invitations_alias_status",
        "game_invitations",
        ["alias", "status"],
    )


def downgrade() -> None:
    op.drop_index("ix_game_invitations_alias_status")
    op.drop_table("game_invitations")
    op.drop_column("games", "started_at")
