"""Runtime settings loaded from environment variables.

Everything account-specific (Cognito pool ID, client ID, ACM cert ARN,
etc.) is read from env at pod start — nothing is baked into the image
or committed to git.
"""
from __future__ import annotations

import os
from dataclasses import dataclass


@dataclass(frozen=True)
class Settings:
    # Postgres — pod-colocated on localhost.
    database_url: str

    # Cognito user pool. The pool ID and app-client ID are needed on
    # the server to verify the ALB-signed OIDC JWT. Domain is used only
    # to build the logout URL.
    cognito_pool_id: str
    cognito_client_id: str
    cognito_domain: str
    cognito_region: str

    # Public site origin (for OIDC callback + logout redirects).
    site_origin: str

    # Admin token for bootstrap admin operations before any user exists.
    # Not the same as an API key.
    admin_token: str

    # Reveal-cache TTL. On first Cognito login, the plain API key is
    # held in-process for this many seconds so the welcome page can
    # ship it to the SPA. After that, it's gone forever.
    reveal_ttl_seconds: int = 24 * 3600

    # Default locale + supported set. Welcome page + SPA both use these.
    default_locale: str = "en"
    supported_locales: tuple[str, ...] = ("en", "zh-CN")

    # Game orchestration (M3).
    openbw_server_image: str = ""
    games_namespace: str = "simsc-games"
    games_host: str = "simsc.agentnumber47.com"
    # ACM cert ARN for the shared ALB (games Ingresses use the same
    # cert as the main site).
    games_acm_cert_arn: str = ""


def _require(key: str) -> str:
    val = os.environ.get(key)
    if not val:
        raise RuntimeError(
            f"required env var {key!r} is not set. "
            f"For local dev, source aws_account_info/simsc-env; "
            f"for k8s, inject via env or a Secret."
        )
    return val


def load_settings() -> Settings:
    return Settings(
        database_url=os.environ.get(
            "DATABASE_URL", "postgresql+psycopg2://simsc:simsc@127.0.0.1:5432/simsc"
        ),
        cognito_pool_id=_require("COGNITO_POOL_ID"),
        cognito_client_id=_require("COGNITO_CLIENT_ID"),
        cognito_domain=_require("COGNITO_DOMAIN"),
        cognito_region=os.environ.get("COGNITO_REGION", "ap-northeast-1"),
        site_origin=os.environ.get("SITE_ORIGIN", "https://simsc.agentnumber47.com"),
        admin_token=os.environ.get("ADMIN_TOKEN", ""),
        openbw_server_image=os.environ.get("OPENBW_SERVER_IMAGE", ""),
        games_namespace=os.environ.get("GAMES_NAMESPACE", "simsc-games"),
        games_host=os.environ.get("GAMES_HOST", "simsc.agentnumber47.com"),
        games_acm_cert_arn=os.environ.get("GAMES_ACM_CERT_ARN", ""),
    )


settings = load_settings()
