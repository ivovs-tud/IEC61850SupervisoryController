"""Resolve HMI publisher and command endpoints from command-line arguments."""

from __future__ import annotations

from collections.abc import Sequence
from urllib.parse import urlsplit


WINDOWS_PUBLISHER_ENDPOINT = "tcp://localhost:5555"
WINDOWS_COMMAND_ENDPOINT = "tcp://localhost:5556"
POSIX_PUBLISHER_ENDPOINT = "ipc:///tmp/supervisory_controller_hmi.sock"
POSIX_COMMAND_ENDPOINT = "ipc:///tmp/supervisory_controller_hmi_cmd.sock"


def resolve_hmi_endpoints(arguments: Sequence[str], platform: str) -> tuple[str, str]:
    if len(arguments) > 2:
        raise ValueError("expected at most a host, or publisher and command endpoints")

    if not arguments:
        if platform == "win32":
            return WINDOWS_PUBLISHER_ENDPOINT, WINDOWS_COMMAND_ENDPOINT
        return POSIX_PUBLISHER_ENDPOINT, POSIX_COMMAND_ENDPOINT

    publisher = arguments[0].strip()
    if not publisher:
        raise ValueError("HMI host or publisher endpoint must not be empty")

    if "://" not in publisher:
        command = arguments[1].strip() if len(arguments) == 2 else f"tcp://{publisher}:5556"
        _require_endpoint(command, "command")
        return f"tcp://{publisher}:5555", command

    _require_endpoint(publisher, "publisher")
    if len(arguments) == 2:
        command = arguments[1].strip()
        _require_endpoint(command, "command")
        return publisher, command

    if publisher.startswith("tcp://"):
        parsed = urlsplit(publisher)
        if not parsed.hostname:
            raise ValueError("TCP publisher endpoint must contain a host")
        host = f"[{parsed.hostname}]" if ":" in parsed.hostname else parsed.hostname
        return publisher, f"tcp://{host}:5556"

    return publisher, POSIX_COMMAND_ENDPOINT


def _require_endpoint(value: str, name: str) -> None:
    if not value or "://" not in value:
        raise ValueError(f"{name} endpoint must include a transport, such as tcp:// or ipc://")
    if not (value.startswith("tcp://") or value.startswith("ipc://")):
        raise ValueError(f"unsupported {name} endpoint transport")
