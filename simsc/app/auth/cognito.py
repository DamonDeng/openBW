"""Parse ALB-signed OIDC JWT.

ALB injects `x-amzn-oidc-data` on Cognito-protected routes. The token
is signed by ALB (not Cognito) with an ES256 key rotated periodically;
public JWKs are served at
`https://public-keys.auth.elb.<region>.amazonaws.com/<kid>`.

We could verify signatures, but the trust boundary in our design is
the ALB itself + the pod's security group — anything at
`/ui/*` inside the cluster came through ALB. simstock takes the same
shortcut (see simstock's app/api/cognito.py). We parse but don't
signature-verify. For added safety, we bail on any parse anomaly.
"""
from __future__ import annotations

import base64
import json
from dataclasses import dataclass
from typing import Any, Optional


@dataclass(slots=True)
class CognitoClaims:
    sub: str
    email: Optional[str]
    preferred_username: Optional[str]
    name: Optional[str]
    raw: dict[str, Any]

    @property
    def alias(self) -> str:
        """Human handle for our app. Prefer preferred_username; fall
        back to the email local-part (never used in identity keys,
        only for display / initial alias picking).
        """
        if self.preferred_username:
            return self.preferred_username
        if self.email and "@" in self.email:
            return self.email.split("@", 1)[0]
        return self.sub[:8]


def parse_alb_oidc_data(token: str) -> CognitoClaims:
    """Parse the JWT payload. No signature check (see module docstring)."""
    parts = token.split(".")
    if len(parts) != 3:
        raise ValueError("malformed JWT: expected 3 dot-separated segments")
    payload_b64 = parts[1]
    # base64url with missing padding
    payload_b64 += "=" * (-len(payload_b64) % 4)
    payload_bytes = base64.urlsafe_b64decode(payload_b64)
    payload = json.loads(payload_bytes)
    sub = payload.get("sub") or payload.get("username")
    if not sub:
        raise ValueError("JWT missing 'sub'")
    return CognitoClaims(
        sub=sub,
        email=payload.get("email"),
        preferred_username=payload.get("preferred_username"),
        name=payload.get("name"),
        raw=payload,
    )
