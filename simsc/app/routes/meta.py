"""Meta / healthcheck routes."""
from __future__ import annotations

from fastapi import APIRouter

from app.core.config import settings

router = APIRouter()


@router.get("/healthz")
def healthz() -> dict:
    return {"ok": True}


@router.get("/api/config")
def public_config() -> dict:
    """Values the SPA + welcome page need to bootstrap. Deliberately
    minimal — no secrets, no ARNs, just user-facing knobs.
    """
    return {
        "default_locale": settings.default_locale,
        "supported_locales": list(settings.supported_locales),
        "site_origin": settings.site_origin,
    }
