"""Closed-loop cmd-result status vocabulary.

Mirror of docs/agent_command_status_codes.md. The integer values are
the wire protocol -- the server writes them into
{"type":"result","status":N,...} frames. Agents that pin their
protocol version get to compare by numeric value, agents that don't
should compare by symbolic name.

Statuses 0..99 are terminal-blob apply-time codes (the server
resolved the command's fate in the sim). 100+ are reserved for
future enqueue-time or lifecycle codes; today the server sends
those as `type:"error"` messages instead. See the doc for the
canonical mapping.
"""

from enum import IntEnum


class Status(IntEnum):
    # 0..99: apply-time outcomes (one per accepted command)
    APPLIED       = 0    # sim's read_action returned true
    REFUSED       = 1    # sim's read_action returned false (validation refused)
    THROWN        = 2    # read_action raised (bwgame::exception)
    SLOT_INACTIVE = 3    # actor's slot went inactive before apply
    NEVER_APPLIED = 4    # game ended / agent disconnected before apply tick

    @property
    def ok(self) -> bool:
        """True iff the command was actually applied by the sim."""
        return self is Status.APPLIED
