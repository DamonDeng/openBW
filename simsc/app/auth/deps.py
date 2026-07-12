"""FastAPI dependencies:

  require_cognito  — pull ALB-signed OIDC claims + resolve/create User row.
                     Used only on /ui/* routes served behind ALB Cognito.
  require_user     — resolve X-API-Key -> User row. Used on /api/*.
  require_admin    — same as require_user + is_admin check.
"""
from __future__ import annotations

from typing import Optional

from fastapi import Depends, Header, HTTPException, status
from sqlalchemy import select
from sqlalchemy.orm import Session

from app.auth.cognito import CognitoClaims, parse_alb_oidc_data
from app.core.security import hash_api_key
from app.db.models import ApiKey, User
from app.db.session import get_db


def get_cognito_claims(
    x_amzn_oidc_data: Optional[str] = Header(None),
) -> CognitoClaims:
    if not x_amzn_oidc_data:
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="missing ALB OIDC header",
        )
    try:
        return parse_alb_oidc_data(x_amzn_oidc_data)
    except (ValueError, KeyError) as e:
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail=f"malformed OIDC token: {e}",
        )


def require_cognito(
    claims: CognitoClaims = Depends(get_cognito_claims),
    db: Session = Depends(get_db),
) -> tuple[CognitoClaims, Optional[User]]:
    """Return (claims, User-or-None). The User may not exist yet on
    first login. Endpoint handles first-login creation via a helper.
    """
    user = db.execute(
        select(User).where(User.cognito_sub == claims.sub)
    ).scalar_one_or_none()
    return claims, user


def require_user(
    x_api_key: Optional[str] = Header(None),
    db: Session = Depends(get_db),
) -> User:
    if not x_api_key:
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="missing X-API-Key",
        )
    h = hash_api_key(x_api_key)
    key = db.execute(
        select(ApiKey).where(ApiKey.key_hash == h)
    ).scalar_one_or_none()
    if key is None or key.revoked_at is not None:
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="invalid API key",
        )
    user = db.get(User, key.user_id)
    if user is None or not user.is_enabled:
        raise HTTPException(status_code=status.HTTP_403_FORBIDDEN, detail="disabled")
    return user


def require_admin(user: User = Depends(require_user)) -> User:
    if not user.is_admin:
        raise HTTPException(status_code=status.HTTP_403_FORBIDDEN, detail="admin only")
    return user
