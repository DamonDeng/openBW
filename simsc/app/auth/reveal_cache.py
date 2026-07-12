"""In-process reveal cache for first-login API keys.

The plain API key must be shown to the user exactly once (welcome page
mirrors it into localStorage). We hold it in a small dict keyed by
Cognito sub with a wall-clock TTL. On pod restart the cache is lost;
that's fine — user just does an admin-reset if they lost the key.

Thread-safety: the FastAPI event loop is single-threaded (uvicorn
default worker); dict access is atomic under GIL. If we ever move to
gunicorn multi-worker, this design breaks and we need Redis.
"""
from __future__ import annotations

import threading
import time
from dataclasses import dataclass
from typing import Optional


@dataclass(slots=True)
class _Entry:
    key: str
    expires_at: float  # unix seconds


class RevealCache:
    def __init__(self, ttl_seconds: int) -> None:
        self._ttl = ttl_seconds
        self._data: dict[str, _Entry] = {}
        self._lock = threading.Lock()

    def put(self, sub: str, plain_key: str) -> None:
        with self._lock:
            self._data[sub] = _Entry(plain_key, time.time() + self._ttl)

    def get(self, sub: str) -> Optional[str]:
        with self._lock:
            entry = self._data.get(sub)
            if entry is None:
                return None
            if entry.expires_at < time.time():
                del self._data[sub]
                return None
            return entry.key

    def clear(self, sub: str) -> None:
        with self._lock:
            self._data.pop(sub, None)
