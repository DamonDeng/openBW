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

import logging
import pathlib

from fastapi import FastAPI
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles

from app.auth.reveal_cache import RevealCache
from app.core.config import settings
from app.routes import api as api_routes
from app.routes import meta as meta_routes
from app.routes import ui as ui_routes

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
def _startup_banner() -> None:
    log.info(
        "simsc-app starting; site=%s cognito=%s pool=%s",
        settings.site_origin, settings.cognito_domain, settings.cognito_pool_id,
    )
