"""games.game_speed

Revision ID: 0005
Revises: 0004
Create Date: 2026-07-14 04:30:00.000000

Add per-game game_speed. Existing rows get server_default='fastest'
(matches openbw_server's baked-in default so behavior is unchanged
for legacy games); the server_default is then dropped so future
inserts must supply a value explicitly via the API/service layer.
"""
from __future__ import annotations

from typing import Sequence, Union

from alembic import op
import sqlalchemy as sa

revision: str = "0005"
down_revision: Union[str, None] = "0004"
branch_labels: Union[str, Sequence[str], None] = None
depends_on: Union[str, Sequence[str], None] = None


def upgrade() -> None:
    op.add_column(
        "games",
        sa.Column(
            "game_speed",
            sa.String(16),
            nullable=False,
            server_default="fastest",
        ),
    )
    op.alter_column("games", "game_speed", server_default=None)


def downgrade() -> None:
    op.drop_column("games", "game_speed")
