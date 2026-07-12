"""Cognito-gated `/ui/*` routes.

These are the only server-side routes protected by ALB Cognito (see
aws_account_info/06_alb_split_spike.md). Everything else is either
public static (SPA) or API-key auth (`/api/*`).

  GET  /ui/whoami           -> identity + one-time API key reveal
  POST /ui/whoami/ack-key   -> clear reveal cache for this session
  GET  /ui/logout           -> ALB Cognito logout redirect chain
"""
from __future__ import annotations

from fastapi import APIRouter, Depends, Request
from fastapi.responses import RedirectResponse
from sqlalchemy.orm import Session

from app.auth.cognito import CognitoClaims
from app.auth.deps import get_cognito_claims
from app.auth.reveal_cache import RevealCache
from app.core.config import settings
from app.db.session import get_db
from app.services.users import resolve_and_provision

router = APIRouter(prefix="/ui")


def _get_reveal_cache(request: Request) -> RevealCache:
    return request.app.state.reveal_cache


@router.get("/whoami")
def whoami(
    request: Request,
    claims: CognitoClaims = Depends(get_cognito_claims),
    db: Session = Depends(get_db),
) -> dict:
    """Return identity + (on first login) the plain API key exactly
    once. Subsequent calls return api_key=None; the SPA has already
    saved the key to localStorage.
    """
    reveal = _get_reveal_cache(request)
    user, plain_key = resolve_and_provision(db, reveal, claims)
    return {
        "alias": user.alias,
        "display_name": user.display_name,
        "email": user.email,
        "is_admin": user.is_admin,
        "is_enabled": user.is_enabled,
        "api_key": plain_key,  # non-null only on the one-time reveal
    }


@router.post("/whoami/ack-key")
def ack_key(
    request: Request,
    claims: CognitoClaims = Depends(get_cognito_claims),
) -> dict:
    _get_reveal_cache(request).clear(claims.sub)
    return {"ok": True}


@router.get("/logout")
def logout() -> RedirectResponse:
    """Kick the user through Cognito's global logout so both the ALB
    session cookie AND the Cognito hosted-UI cookie are cleared. Then
    the browser bounces to /ui/logged-out below (which is a small
    "you're signed out" page — also Cognito-protected so the next
    visit forces a fresh login).
    """
    logout_url = (
        f"https://{settings.cognito_domain}.auth.{settings.cognito_region}.amazoncognito.com"
        f"/logout?client_id={settings.cognito_client_id}"
        f"&logout_uri={settings.site_origin}/ui/logged-out"
    )
    return RedirectResponse(logout_url, status_code=302)


@router.get("/logged-out")
def logged_out() -> dict:
    """Landing after Cognito logout completes. Anything served here is
    behind Cognito, so hitting it forces re-auth — that's the whole
    point (a returning user should re-sign-in cleanly).
    """
    return {"message": "signed out"}
