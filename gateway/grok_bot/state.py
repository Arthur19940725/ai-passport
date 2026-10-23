"""Pure, bounded state machine. Call only on the server's single event loop."""

from collections import OrderedDict, deque
from dataclasses import dataclass, field
import re
import time
from typing import Any, Callable
from uuid import uuid4

from .config import DEVICE_ID_PATTERN

COMMAND_TTL_SECONDS = 30.0
STATUS_STALE_SECONDS = 15.0
MAX_PENDING = 8
MAX_HISTORY = 64
UUID_PATTERN = re.compile(r"[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}\Z")
BOOT_PATTERN = re.compile(r"[0-9a-f]{16}\Z")


class ProtocolError(ValueError):
    """Invalid device payload, without reflecting private data into logs."""


class CommandRejected(ValueError):
    """A command could not be queued; its code is safe to return to MCP."""


def _integer(value: Any, minimum: int, maximum: int) -> bool:
    return type(value) is int and minimum <= value <= maximum


def validate_message(text: Any) -> None:
    if not isinstance(text, str) or not 1 <= len(text) <= 160:
        raise CommandRejected("invalid_text_length")
    if any(char != "\n" and not " " <= char <= "~" for char in text):
        raise CommandRejected("unsupported_text_use_printable_ascii_or_newline")


def validate_poll(data: Any) -> dict[str, Any]:
    fields = {
        "version", "device_id", "boot_id", "remote_enabled", "battery_percent",
        "battery_mv", "brightness", "uptime_ms", "free_heap", "last_result",
    }
    if type(data) is not dict or set(data) != fields:
        raise ProtocolError("invalid_poll_fields")
    if type(data["version"]) is not int or data["version"] != 1:
        raise ProtocolError("unsupported_version")
    if not isinstance(data["device_id"], str) or not DEVICE_ID_PATTERN.fullmatch(data["device_id"]):
        raise ProtocolError("invalid_device_id")
    if not isinstance(data["boot_id"], str) or not BOOT_PATTERN.fullmatch(data["boot_id"]):
        raise ProtocolError("invalid_boot_id")
    if type(data["remote_enabled"]) is not bool:
        raise ProtocolError("invalid_remote_enabled")
    ranges = {
        "battery_percent": (-1, 100), "battery_mv": (-1, 6000),
        "brightness": (10, 100), "uptime_ms": (0, 2**63 - 1), "free_heap": (0, 2**32 - 1),
    }
    if any(not _integer(data[key], *bounds) for key, bounds in ranges.items()):
        raise ProtocolError("invalid_telemetry_range")
    result = data["last_result"]
    if type(result) is not dict or set(result) != {"id", "ok", "error"}:
        raise ProtocolError("invalid_result_fields")
    if not isinstance(result["id"], str) or (result["id"] and not UUID_PATTERN.fullmatch(result["id"])):
        raise ProtocolError("invalid_result_id")
    if type(result["ok"]) is not bool or not isinstance(result["error"], str) or len(result["error"]) > 64:
        raise ProtocolError("invalid_result")
    # Firmware errors are short machine codes, never arbitrary model instructions.
    if any(not " " <= char <= "~" for char in result["error"]):
        raise ProtocolError("invalid_result_error")
    return {**data, "last_result": dict(result)}


@dataclass
class Command:
    id: str
    type: str
    boot_id: str
    created_at: float
    payload: dict[str, Any] = field(repr=False)
    status: str = "queued"
    sent: bool = False
    error: str | None = None

    def result(self) -> dict[str, Any]:
        execution = "unconfirmed" if self.sent else "not_sent"
        if self.status == "acknowledged":
            execution = "confirmed"
        elif self.status == "device_error":
            execution = "failed"
        return {"id": self.id, "status": self.status, "execution": execution, "error": self.error}


class DeviceState:
    def __init__(self, device_id: str, clock: Callable[[], float] = time.monotonic):
        self.device_id = device_id
        self._clock = clock
        self._boot_id: str | None = None
        self._retired_boots: deque[str] = deque(maxlen=16)
        self._last_seen: float | None = None
        self._telemetry: dict[str, Any] | None = None
        self._pending: OrderedDict[str, Command] = OrderedDict()
        self._history: OrderedDict[str, dict[str, Any]] = OrderedDict()
        self._inflight: str | None = None

    def _finish(self, command: Command, status: str, error: str | None = None) -> None:
        command.status = status
        command.error = error
        self._history[command.id] = command.result()
        self._pending.pop(command.id)
        command.payload.clear()
        if self._inflight == command.id:
            self._inflight = None
        while len(self._history) > MAX_HISTORY:
            self._history.popitem(last=False)

    def expire(self) -> None:
        now = self._clock()
        for command in list(self._pending.values()):
            if now - command.created_at >= COMMAND_TTL_SECONDS:
                self._finish(command, "expired", "ack_timeout" if command.sent else "queue_timeout")

    def _cancel_all(self, reason: str) -> None:
        for command in list(self._pending.values()):
            self._finish(command, "cancelled", reason)

    def status(self) -> dict[str, Any]:
        self.expire()
        age = None if self._last_seen is None else max(0.0, self._clock() - self._last_seen)
        online = age is not None and age <= STATUS_STALE_SECONDS
        return {
            "device_id": self.device_id,
            "online": online,
            "stale": not online,
            "last_seen_seconds_ago": None if age is None else round(age, 1),
            "remote_enabled": bool(online and self._telemetry and self._telemetry["remote_enabled"]),
            "telemetry": None if self._telemetry is None else dict(self._telemetry),
            "pending_commands": len(self._pending),
        }

    def enqueue(self, kind: str, *, text: str | None = None, brightness: int | None = None) -> dict[str, Any]:
        status = self.status()
        if not status["online"]:
            raise CommandRejected("device_offline")
        if not status["remote_enabled"]:
            raise CommandRejected("remote_disabled")
        if len(self._pending) >= MAX_PENDING:
            raise CommandRejected("queue_full")
        payload: dict[str, Any] = {}
        if kind == "show_message":
            validate_message(text)
            payload["text"] = text
        elif kind == "set_backlight":
            if not _integer(brightness, 10, 100):
                raise CommandRejected("invalid_brightness")
            payload["brightness"] = brightness
        elif kind != "clear_message":
            raise CommandRejected("unsupported_command")
        # An online state has necessarily accepted a poll with a validated boot ID.
        assert self._boot_id is not None
        command = Command(str(uuid4()), kind, self._boot_id, self._clock(), payload)
        self._pending[command.id] = command
        return command.result()

    def command_status(self, command_id: str) -> dict[str, Any]:
        self.expire()
        if not isinstance(command_id, str) or not UUID_PATTERN.fullmatch(command_id):
            raise CommandRejected("invalid_command_id")
        if command_id in self._pending:
            return self._pending[command_id].result()
        if command_id in self._history:
            return dict(self._history[command_id])
        return {"id": command_id, "status": "unknown", "execution": "unconfirmed", "error": "not_in_memory"}

    def poll(self, raw_data: Any) -> dict[str, Any]:
        data = validate_poll(raw_data)
        if data["device_id"] != self.device_id:
            raise ProtocolError("wrong_device")
        if data["boot_id"] in self._retired_boots:
            raise ProtocolError("retired_boot")
        self.expire()
        if self._boot_id != data["boot_id"]:
            self._cancel_all("device_rebooted")
            if self._boot_id is not None:
                self._retired_boots.append(self._boot_id)
            self._boot_id = data["boot_id"]
        # Only the exact in-flight ID in the same boot session can be ACKed.
        # Previous/unknown IDs are ignored, allowing reconnect after server restart.
        result = data["last_result"]
        if self._inflight and result["id"] == self._inflight:
            command = self._pending[self._inflight]
            if command.boot_id == self._boot_id:
                self._finish(
                    command, "acknowledged" if result["ok"] else "device_error",
                    None if result["ok"] else (result["error"] or "device_rejected"),
                )
        self._last_seen = self._clock()
        self._telemetry = {key: data[key] for key in (
            "boot_id", "remote_enabled", "battery_percent", "battery_mv", "brightness", "uptime_ms", "free_heap",
        )}
        if not data["remote_enabled"]:
            self._cancel_all("remote_disabled")
            return {"version": 1, "command": None}
        if not self._pending:
            return {"version": 1, "command": None}
        command = next(iter(self._pending.values()))
        command.status = "dispatched"
        command.sent = True
        self._inflight = command.id
        return {"version": 1, "command": {"id": command.id, "type": command.type, **command.payload}}
