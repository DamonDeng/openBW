"""Public /api/* routes. Auth is X-API-Key ONLY — never Cognito."""
from __future__ import annotations

from datetime import datetime, timezone

from fastapi import APIRouter, Body, Depends, HTTPException, status
from pydantic import BaseModel, Field
from sqlalchemy import select
from sqlalchemy.orm import Session

from app.auth.deps import require_admin, require_user
from app.core.security import generate_api_key, hash_api_key
from app.db.models import ApiKey, User
from app.db.session import get_db

router = APIRouter(prefix="/api")


class ProfileOut(BaseModel):
    alias: str
    display_name: str | None
    email: str | None
    is_admin: bool


class KeyOut(BaseModel):
    id: int
    label: str | None
    created_at: datetime
    revoked_at: datetime | None


class KeyCreated(BaseModel):
    id: int
    label: str | None
    plain_key: str  # shown once
    created_at: datetime


class UserOut(BaseModel):
    id: int
    alias: str
    display_name: str | None
    email: str | None
    is_admin: bool
    is_enabled: bool
    created_at: datetime


@router.get("/me/profile", response_model=ProfileOut)
def me_profile(user: User = Depends(require_user)) -> ProfileOut:
    return ProfileOut(
        alias=user.alias,
        display_name=user.display_name,
        email=user.email,
        is_admin=user.is_admin,
    )


@router.get("/me/keys", response_model=list[KeyOut])
def me_keys(
    user: User = Depends(require_user),
    db: Session = Depends(get_db),
) -> list[KeyOut]:
    rows = db.execute(
        select(ApiKey).where(ApiKey.user_id == user.id).order_by(ApiKey.created_at.desc())
    ).scalars().all()
    return [
        KeyOut(id=r.id, label=r.label, created_at=r.created_at, revoked_at=r.revoked_at)
        for r in rows
    ]


class KeyCreateIn(BaseModel):
    label: str | None = Field(default=None, max_length=64)


@router.post("/me/keys", response_model=KeyCreated)
def me_keys_create(
    body: KeyCreateIn = Body(default_factory=KeyCreateIn),
    user: User = Depends(require_user),
    db: Session = Depends(get_db),
) -> KeyCreated:
    # Cap active keys per user.
    active = db.execute(
        select(ApiKey).where(ApiKey.user_id == user.id, ApiKey.revoked_at.is_(None))
    ).scalars().all()
    if len(active) >= 5:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, "max 5 active keys per user")
    plain = generate_api_key()
    row = ApiKey(user_id=user.id, key_hash=hash_api_key(plain), label=body.label)
    db.add(row)
    db.commit()
    return KeyCreated(id=row.id, label=row.label, plain_key=plain, created_at=row.created_at)


@router.delete("/me/keys/{key_id}")
def me_keys_revoke(
    key_id: int,
    user: User = Depends(require_user),
    db: Session = Depends(get_db),
) -> dict:
    row = db.get(ApiKey, key_id)
    if row is None or row.user_id != user.id:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "no such key")
    if row.revoked_at is not None:
        return {"ok": True, "already_revoked": True}
    row.revoked_at = datetime.now(timezone.utc)
    db.commit()
    return {"ok": True}


# ---- admin ----


@router.get("/admin/users", response_model=list[UserOut])
def admin_list_users(
    _admin: User = Depends(require_admin),
    db: Session = Depends(get_db),
) -> list[UserOut]:
    rows = db.execute(select(User).order_by(User.created_at.desc())).scalars().all()
    return [
        UserOut(
            id=u.id, alias=u.alias, display_name=u.display_name, email=u.email,
            is_admin=u.is_admin, is_enabled=u.is_enabled, created_at=u.created_at,
        )
        for u in rows
    ]


class AdminGrantIn(BaseModel):
    is_admin: bool


@router.post("/admin/users/{user_id}/admin", response_model=UserOut)
def admin_set_admin(
    user_id: int,
    body: AdminGrantIn,
    admin: User = Depends(require_admin),
    db: Session = Depends(get_db),
) -> UserOut:
    if user_id == admin.id and not body.is_admin:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, "cannot demote yourself")
    target = db.get(User, user_id)
    if target is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "no such user")
    target.is_admin = body.is_admin
    db.commit()
    return UserOut(
        id=target.id, alias=target.alias, display_name=target.display_name,
        email=target.email, is_admin=target.is_admin, is_enabled=target.is_enabled,
        created_at=target.created_at,
    )
