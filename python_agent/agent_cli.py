"""Shared CLI plumbing for agent scripts.

Every agent in python_agent/agents/ uses argparse with a nearly-identical
prelude: positional api_key + --host + --port (and sometimes --url).
This module centralizes that so:

  * The Qt lobby (simsc_desktop) can rely on a uniform launch contract
    across every agent it invokes -- the three flags --url / --api-key /
    --race, nothing else.
  * Adding agent-specific flags stays local to the agent file; the
    boilerplate is a one-line call.
  * Back-compat with legacy invocations (positional key + --host/--port)
    is preserved so the docs, runbooks, and my own muscle memory don't
    break.

The launch-contract standard the lobby speaks is:

    <agent-executable> --url <base-ws-url> --api-key <key> --race <r>

where <base-ws-url> is the ws:// or wss:// URL WITHOUT the ?key= query
string; the agent appends it from --api-key. --race is always concrete
(zerg / terran / protoss) -- openbw_server rejects "random" at CLI parse,
and the lobby resolves random slot races before the attach button is
even clickable. Single-race agents are free to ignore --race entirely;
the flag exists so every agent presents the same shape.

Typical usage inside an agent's argparse setup:

    from python_agent.agent_cli import add_standard_args, resolve_connection

    p = argparse.ArgumentParser(...)
    add_standard_args(p)              # <-- one line, replaces four
    p.add_argument("--interval-sec", ...)
    ... agent-specific flags ...
    args = p.parse_args()
    api_key, client_kwargs = resolve_connection(args)
    # client_kwargs is ready to pass to Client(**client_kwargs).
"""
from __future__ import annotations

import argparse
from typing import Tuple


VALID_RACES = ("zerg", "terran", "protoss")


def add_standard_args(parser: argparse.ArgumentParser) -> None:
    """Register the standard launch-contract flags on `parser`.

    Adds:
      - `--url <base-ws-url>`     (new preferred connection form)
      - `--api-key <key>`         (new preferred key form)
      - `--race <r>`              (new; single-race agents may ignore)
      - `api_key` (positional)    (legacy; still accepted)
      - `--host <host>`           (legacy; ignored if --url set)
      - `--port <port>`           (legacy; ignored if --url set)

    Legacy args stay so existing scripts / docs / shell one-liners keep
    working. New callers (the Qt lobby, workshop attendees writing new
    agents) should use --url + --api-key + --race only.
    """
    # New standard trio.
    parser.add_argument(
        "--url", default=None,
        help="base WS URL (ws://host:port/agent or wss://host/game/<id>/agent). "
             "Overrides --host/--port. The agent appends '?key=<api_key>'.",
    )
    parser.add_argument(
        "--api-key", dest="api_key_flag", default=None,
        help="API key for the slot. Preferred over the positional form.",
    )
    parser.add_argument(
        "--race", default=None,
        choices=VALID_RACES,
        help="Concrete race for the slot. Single-race agents may ignore this; "
             "it exists so every agent presents the same launch signature.",
    )
    # Legacy (positional key + host/port). nargs='?' so --api-key alone works.
    parser.add_argument(
        "api_key", nargs="?", default=None,
        help="[legacy] API key. Prefer --api-key.",
    )
    parser.add_argument(
        "--host", default="127.0.0.1",
        help="[legacy] server host. Ignored when --url is set.",
    )
    parser.add_argument(
        "--port", type=int, default=6113,
        help="[legacy] server port. Ignored when --url is set.",
    )


def resolve_connection(args: argparse.Namespace) -> Tuple[str, dict]:
    """Turn the parsed args into (api_key, client_kwargs).

    Order of precedence:
      * api_key: --api-key wins over the positional. Missing both -> error.
      * connection: --url wins over --host/--port.

    Returns:
      api_key: the resolved key string (also passed inside client_kwargs).
      client_kwargs: ready to splat into `Client(**client_kwargs)`.
    """
    api_key = args.api_key_flag or args.api_key
    if not api_key:
        raise SystemExit(
            "error: no API key. Pass --api-key <key> (preferred) or as the "
            "positional argument (legacy)."
        )
    if args.url:
        return api_key, {"api_key": api_key, "url": args.url}
    return api_key, {
        "api_key": api_key, "host": args.host, "port": args.port,
    }
