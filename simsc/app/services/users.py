"""User lifecycle: first-login provisioning, key minting.

Implements the "login = ready" pattern from simstock (see
aws_account_info/04_m2_plan.md). On first Cognito login:
  1. Insert users row keyed by cognito_sub
  2. Mint a fresh API key, store hash
  3. Stash plain key in reveal cache with 24h TTL
  4. Return the plain key so the welcome page's whoami response can
     mirror it into localStorage

For pre-seeded users (row created out-of-band by admin), same path:
if the user has NO active key at login, mint one on the spot.
"""
from __future__ import annotations

from typing import Optional

from sqlalchemy import and_, select
from sqlalchemy.orm import Session

from app.auth.cognito import CognitoClaims
from app.auth.reveal_cache import RevealCache
from app.core.security import generate_api_key, hash_api_key
from app.db.models import ApiKey, User


def _pick_unique_alias(db: Session, base: str) -> str:
    """Ensure alias uniqueness by appending -2, -3, ... on collision."""
    if not db.execute(select(User).where(User.alias == base)).first():
        return base
    for i in range(2, 1000):
        candidate = f"{base}-{i}"
        if not db.execute(select(User).where(User.alias == candidate)).first():
            return candidate
    raise RuntimeError(f"could not find unique alias for base={base!r}")


def resolve_and_provision(
    db: Session,
    reveal_cache: RevealCache,
    claims: CognitoClaims,
) -> tuple[User, Optional[str]]:
    """Look up (or create) a User for the given Cognito claims.

    Returns (user, plain_key_or_None). The plain_key is non-None ONLY
    when a fresh key was minted this call (first login for this user,
    or pre-seeded user with no active key).
    """
    user = db.execute(
        select(User).where(User.cognito_sub == claims.sub)
    ).scalar_one_or_none()

    if user is None:
        # First-ever login for this Cognito identity.
        alias = _pick_unique_alias(db, claims.alias)
        user = User(
            cognito_sub=claims.sub,
            alias=alias,
            email=claims.email,
            display_name=claims.name,
        )
        db.add(user)
        db.flush()  # get user.id
        plain = mint_key(db, user, label="first-login")
        db.commit()
        reveal_cache.put(claims.sub, plain)
        return user, plain

    # User exists. If they have no active key, mint one so "login =
    # ready" still holds for pre-seeded rows.
    has_active = db.execute(
        select(ApiKey.id).where(
            and_(ApiKey.user_id == user.id, ApiKey.revoked_at.is_(None))
        ).limit(1)
    ).first()
    if not has_active:
        plain = mint_key(db, user, label="post-seed")
        db.commit()
        reveal_cache.put(claims.sub, plain)
        return user, plain

    # Existing user with existing key. Reveal cache MAY still hold
    # the plain key from an earlier login this pod; return it if so.
    plain = reveal_cache.get(claims.sub)
    return user, plain


def mint_key(db: Session, user: User, label: str) -> str:
    """Insert a new active API key for `user` and return the plain
    text. Caller is responsible for `db.commit()`."""
    plain = generate_api_key()
    key = ApiKey(user_id=user.id, key_hash=hash_api_key(plain), label=label)
    db.add(key)
    return plain
