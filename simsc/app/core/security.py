"""Password / API-key primitives.

API keys are random 32-byte URL-safe strings prefixed `sk-` (matching
the openbw agent-key format). We store only the SHA-256 hash; the
plain key is shown once to the user on issue.
"""
from __future__ import annotations

import hashlib
import hmac
import secrets

KEY_PREFIX = "sk-"


def generate_api_key() -> str:
    """32 bytes of urandom, base64url-encoded, `sk-` prefixed."""
    raw = secrets.token_urlsafe(32)
    return f"{KEY_PREFIX}{raw}"


def hash_api_key(key: str) -> bytes:
    return hashlib.sha256(key.encode("utf-8")).digest()


def ct_eq(a: bytes, b: bytes) -> bool:
    """Constant-time comparison."""
    return hmac.compare_digest(a, b)
