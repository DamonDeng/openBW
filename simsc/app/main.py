"""FastAPI entry point.

Routing map (see aws_account_info/04_m2_plan.md):

  /                       welcome.html  (Cognito-gated at ALB)
  /ui/whoami              JSON          (Cognito-gated at ALB)
  /ui/whoami/ack-key      JSON          (Cognito-gated at ALB)
  /ui/logout              redirect      (Cognito-gated at ALB)
  /ui/logged-out          JSON          (Cognito-gated at ALB)
  /healthz                JSON          (public)
  /api/config             JSON          (public — bootstrap)
  /api/me/*               JSON          (X-API-Key)
  /api/admin/*            JSON          (X-API-Key + is_admin)
  /simscapp/*             static        (public)
  /simscadmin/*           static        (public)
  /locales/<code>.json    static        (public — used by both SPA
                                         and welcome page)
"""
from __future__ import annotations

import asyncio
import logging
import pathlib

from fastapi import FastAPI
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles

from app.auth.reveal_cache import RevealCache
from app.core.config import settings
from app.db.session import SessionLocal
from app.routes import api as api_routes
from app.routes import games as games_routes
from app.routes import meta as meta_routes
from app.routes import ui as ui_routes
from app.services import games as games_service

logging.basicConfig(
    level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s"
)
log = logging.getLogger("simsc")

_STATIC_ROOT = pathlib.Path(__file__).parent / "static"

app = FastAPI(title="simsc control server", version="m2")
app.state.reveal_cache = RevealCache(ttl_seconds=settings.reveal_ttl_seconds)

app.include_router(meta_routes.router)
app.include_router(ui_routes.router)
app.include_router(api_routes.router)
app.include_router(games_routes.router)
app.include_router(games_routes.admin_router)
app.include_router(games_routes.users_router)

# Static: SPAs + locale bundles.
app.mount("/simscapp", StaticFiles(directory=_STATIC_ROOT / "simscapp", html=True), name="simscapp")
app.mount("/simscadmin", StaticFiles(directory=_STATIC_ROOT / "simscadmin", html=True), name="simscadmin")
app.mount("/locales", StaticFiles(directory=_STATIC_ROOT / "locales"), name="locales")


@app.get("/")
def welcome_page() -> FileResponse:
    """The welcome page. Served from FastAPI so the ALB Cognito header
    is available on same-origin fetches to /ui/whoami.
    """
    return FileResponse(_STATIC_ROOT / "welcome.html")


@app.on_event("startup")
async def _startup_banner() -> None:
    log.info(
        "simsc-app starting; site=%s cognito=%s pool=%s",
        settings.site_origin, settings.cognito_domain, settings.cognito_pool_id,
    )
    # Sweep expired invitations every 60s. Runs forever until the pod
    # dies. Kept in-process — no external scheduler needed at M4 scale.
    async def _sweeper():
        while True:
            try:
                await asyncio.sleep(60)
                db = SessionLocal()
                try:
                    n = games_service.sweep_expired(db)
                    if n:
                        log.info("swept %d expired game(s)", n)
                finally:
                    db.close()
            except Exception as e:
                log.exception("sweeper error: %s", e)
    asyncio.create_task(_sweeper())
