"""users.cognito_sub -> nullable, partial-unique-when-not-null

Revision ID: 0004
Revises: 0003
Create Date: 2026-07-14 03:00:00.000000

Allow bot-mode User rows created via POST /api/admin/users to have
NULL cognito_sub. Postgres partial unique index preserves the
"unique when set" invariant so a real Cognito login can still be
linked without collision.
"""
from __future__ import annotations

from typing import Sequence, Union

from alembic import op
import sqlalchemy as sa

revision: str = "0004"
down_revision: Union[str, None] = "0003"
branch_labels: Union[str, Sequence[str], None] = None
depends_on: Union[str, Sequence[str], None] = None


def upgrade() -> None:
    op.drop_constraint("uq_users_cognito_sub", "users", type_="unique")
    op.alter_column(
        "users", "cognito_sub",
        existing_type=sa.String(64),
        nullable=True,
    )
    op.create_index(
        "uq_users_cognito_sub_not_null",
        "users",
        ["cognito_sub"],
        unique=True,
        postgresql_where=sa.text("cognito_sub IS NOT NULL"),
    )


def downgrade() -> None:
    op.drop_index("uq_users_cognito_sub_not_null", table_name="users")
    # Note: if there are rows with NULL cognito_sub at downgrade time,
    # this will fail. Operator must handle that manually (assign a
    # sub or delete those rows) before running `alembic downgrade`.
    op.alter_column(
        "users", "cognito_sub",
        existing_type=sa.String(64),
        nullable=False,
    )
    op.create_unique_constraint(
        "uq_users_cognito_sub", "users", ["cognito_sub"]
    )
