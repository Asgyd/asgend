"""ASGEND Remote Control Plane server (API v1).

The server is a pure control plane: it stores and routes tasks, commands,
events and artifacts between clients and registered Execution Nodes. It never
runs an orchestrator, models, tools, shells or user code.
"""

__version__ = "1.0.0"
API_VERSION = 1
