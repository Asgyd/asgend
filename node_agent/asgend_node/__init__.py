"""ASGEND Execution Node agent.

Connects a local ASGEND core (line-delimited JSON over TCP or a Unix socket) to a
remote ASGEND Control Plane (HTTPS, contract v1 section 7). The agent never executes
anything itself: it only relays commands to the core and reports state back.
"""

__version__ = "0.1.0"
PROTOCOL_VERSION = 1
