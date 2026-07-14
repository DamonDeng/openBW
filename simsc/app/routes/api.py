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
from app.services.users import mint_key

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
    cognito_sub: str | None
    is_admin: bool
    is_enabled: bool
    created_at: datetime


class UserCreateIn(BaseModel):
    alias: str = Field(..., min_length=1, max_length=64)
    display_name: str | None = Field(default=None, max_length=255)
    email: str | None = Field(default=None, max_length=255)
    is_admin: bool = False


class UserCreatedOut(UserOut):
    plain_key: str  # shown once


class UserPatchIn(BaseModel):
    alias: str | None = Field(default=None, min_length=1, max_length=64)
    display_name: str | None = Field(default=None, max_length=255)
    email: str | None = Field(default=None, max_length=255)
    is_enabled: bool | None = None


class LinkCognitoIn(BaseModel):
    cognito_sub: str = Field(..., min_length=1, max_length=64)
    force: bool = False


def _user_out(u: User) -> UserOut:
    return UserOut(
        id=u.id, alias=u.alias, display_name=u.display_name, email=u.email,
        cognito_sub=u.cognito_sub, is_admin=u.is_admin,
        is_enabled=u.is_enabled, created_at=u.created_at,
    )


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
    return [_user_out(u) for u in rows]


@router.get("/admin/users/{user_id}", response_model=UserOut)
def admin_get_user(
    user_id: int,
    _admin: User = Depends(require_admin),
    db: Session = Depends(get_db),
) -> UserOut:
    target = db.get(User, user_id)
    if target is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "no such user")
    return _user_out(target)


@router.post("/admin/users", response_model=UserCreatedOut)
def admin_create_user(
    body: UserCreateIn,
    _admin: User = Depends(require_admin),
    db: Session = Depends(get_db),
) -> UserCreatedOut:
    # Alias must be unique — cheap pre-check for a nicer error than
    # bubbling a DB IntegrityError.
    if db.execute(select(User).where(User.alias == body.alias)).first():
        raise HTTPException(status.HTTP_409_CONFLICT, f"alias {body.alias!r} already in use")
    user = User(
        alias=body.alias,
        display_name=body.display_name,
        email=body.email,
        is_admin=body.is_admin,
        cognito_sub=None,
    )
    db.add(user)
    db.flush()  # get user.id before minting the key
    plain = mint_key(db, user, label="admin-created")
    db.commit()
    return UserCreatedOut(**_user_out(user).model_dump(), plain_key=plain)


@router.patch("/admin/users/{user_id}", response_model=UserOut)
def admin_patch_user(
    user_id: int,
    body: UserPatchIn,
    _admin: User = Depends(require_admin),
    db: Session = Depends(get_db),
) -> UserOut:
    target = db.get(User, user_id)
    if target is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "no such user")
    if body.alias is not None and body.alias != target.alias:
        clash = db.execute(select(User).where(User.alias == body.alias)).first()
        if clash:
            raise HTTPException(status.HTTP_409_CONFLICT, f"alias {body.alias!r} already in use")
        target.alias = body.alias
    if body.display_name is not None:
        target.display_name = body.display_name
    if body.email is not None:
        target.email = body.email
    if body.is_enabled is not None:
        target.is_enabled = body.is_enabled
    db.commit()
    return _user_out(target)


@router.post("/admin/users/{user_id}/link-cognito", response_model=UserOut)
def admin_link_cognito(
    user_id: int,
    body: LinkCognitoIn,
    _admin: User = Depends(require_admin),
    db: Session = Depends(get_db),
) -> UserOut:
    target = db.get(User, user_id)
    if target is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "no such user")
    other = db.execute(
        select(User).where(User.cognito_sub == body.cognito_sub, User.id != user_id)
    ).scalar_one_or_none()
    if other is not None:
        raise HTTPException(
            status.HTTP_409_CONFLICT,
            f"cognito_sub already linked to user id={other.id} alias={other.alias!r}",
        )
    if target.cognito_sub is not None and target.cognito_sub != body.cognito_sub and not body.force:
        raise HTTPException(
            status.HTTP_409_CONFLICT,
            f"user id={user_id} already linked to a different cognito_sub; "
            "pass force=true to overwrite",
        )
    target.cognito_sub = body.cognito_sub
    db.commit()
    return _user_out(target)


@router.post("/admin/users/{user_id}/unlink-cognito", response_model=UserOut)
def admin_unlink_cognito(
    user_id: int,
    _admin: User = Depends(require_admin),
    db: Session = Depends(get_db),
) -> UserOut:
    target = db.get(User, user_id)
    if target is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "no such user")
    target.cognito_sub = None
    db.commit()
    return _user_out(target)


@router.delete("/admin/users/{user_id}")
def admin_delete_user(
    user_id: int,
    admin: User = Depends(require_admin),
    db: Session = Depends(get_db),
) -> dict:
    if user_id == admin.id:
        raise HTTPException(status.HTTP_400_BAD_REQUEST, "cannot delete yourself")
    target = db.get(User, user_id)
    if target is None:
        raise HTTPException(status.HTTP_404_NOT_FOUND, "no such user")
    target.is_enabled = False
    db.commit()
    return {"ok": True, "soft_deleted": True}


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
    return _user_out(target)
