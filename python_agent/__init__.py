"""Reference Python agent implementation for the openBW workshop server.

Two layers:
  python_agent.client  -- thin wrapper over the WebSocket JSON API.
                          One method per server message type. No cleverness.
  python_agent.enums   -- name <-> id lookups loaded from agent_reference/.

Two sample agents in python_agent.agents.* demonstrate the pattern:
  random_walk -- move idle workers to random points; smallest possible loop.
  miner       -- send idle workers to gather from the nearest mineral field.

Workshop attendees are expected to fork this package or copy pieces into
their own project. Nothing here is imported by the C++ server; it's all
client-side.
"""

__all__ = ["client", "enums"]
