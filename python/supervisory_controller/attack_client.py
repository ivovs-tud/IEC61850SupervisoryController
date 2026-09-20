# SPDX-License-Identifier: Apache-2.0
# Adapted from ivovs-tud/HackAWindFarm; substantially modified for packaging,
# shared protocol types, dependency injection, and deterministic lifecycle.

"""Attack client with a finite, testable lifecycle.

This module adapts the participant-facing API from the separately distributed
HackAWindFarm AttackInterface.py. Protocol encoding is provided by
attack_protocol rather than duplicated here.
"""

from __future__ import annotations

import logging
import math
import threading
import time
from collections.abc import Callable, Sequence
from types import TracebackType
from typing import Any

import zmq

from .attack_protocol import (
    AtDataMessage,
    CfgDataMessage,
    ControlSignal,
    CtDataMessage,
    HeartbeatMessage,
    AttackMessage,
    AttackProtocolError,
    RqDataMessage,
    ReleaseMessage,
    TxDataMessage,
    TxDataType,
    parse_message,
)

AttackFunction = Callable[[dict[str, list[float]], dict[str, list[float]], int], None]


class AttackInterfaceError(RuntimeError):
    """Base error raised by the attack client."""


class AttackClient:
    """Client for the controller's attack interface."""

    INTERVAL_SECONDS = 0.01
    ATTACK_INTERVAL_SECONDS = 0.1
    ZEROMQ_MAX_MESSAGES = 10
    RECV_TIMEOUT_MS = 500
    SEND_TIMEOUT_MS = 500
    HEARTBEAT_INTERVAL_SECONDS = 0.2

    SIGNAL_TYPES = {
        "Wind Speed": TxDataType.TX_WS,
        "Wind Direction": TxDataType.TX_WD,
        "Rotor Speed": TxDataType.TX_RPM,
        "Yaw": TxDataType.TX_YAW,
        "Blade Pitch": TxDataType.TX_PTCH,
        "Power": TxDataType.TX_PW,
        "Yaw Setpoint": TxDataType.TX_SPT_YAW,
        "Power Setpoint": TxDataType.TX_SPT_PWR,
        "Generator Torque": TxDataType.TX_GENTORQ,
    }
    _TEXT2TYPE = SIGNAL_TYPES
    _TYPE2TEXT = {value: key for key, value in SIGNAL_TYPES.items()}
    _AttackInterfaceExcept = AttackInterfaceError

    def __init__(
        self,
        num_turbines: int = 9,
        *,
        socket: Any | None = None,
        context: Any | None = None,
        wall_time_ms: Callable[[], int] | None = None,
        monotonic: Callable[[], float] | None = None,
    ) -> None:
        if num_turbines <= 0:
            raise ValueError("num_turbines must be positive")

        self.num_turbines = num_turbines
        self.server_ip: str | None = None
        self.port: int | None = None
        self.session_label = ""

        self._context = context
        self._owns_context = False
        self._socket = socket
        if self._socket is None:
            self._socket = self._create_zmq_socket(context)

        self._wall_time_ms = wall_time_ms or (lambda: time.time_ns() // 1_000_000)
        self._monotonic = monotonic or time.monotonic
        self._stop_event = threading.Event()
        self._attack_thread: threading.Thread | None = None
        self._attack_func: AttackFunction | None = None
        self._running = False
        self._closed = False
        self._configured = False
        self._next_heartbeat_at = float("inf")

        self._tap_cfg = self._empty_control_map()
        self._fdi_cfg = self._empty_control_map()
        self.last_received = self._empty_value_map()
        self.fdi_next = self._empty_value_map()

    def _create_zmq_socket(self, context: Any | None) -> Any:
        if context is None:
            context = zmq.Context()
            self._owns_context = True
        self._context = context
        socket = context.socket(zmq.PAIR)
        socket.setsockopt(zmq.SNDHWM, self.ZEROMQ_MAX_MESSAGES)
        socket.setsockopt(zmq.RCVTIMEO, self.RECV_TIMEOUT_MS)
        socket.setsockopt(zmq.SNDTIMEO, self.SEND_TIMEOUT_MS)
        return socket

    def _empty_control_map(self) -> dict[str, list[bool]]:
        return {
            name: [False] * self.num_turbines
            for name in self.SIGNAL_TYPES
        }

    def _empty_value_map(self) -> dict[str, list[float]]:
        return {
            name: [float("nan")] * self.num_turbines
            for name in self.SIGNAL_TYPES
        }

    @property
    def running(self) -> bool:
        return self._running and not self._stop_event.is_set()

    @property
    def closed(self) -> bool:
        return self._closed

    def connect(self, server_ip: str, port: int) -> None:
        self._require_open()
        if not server_ip:
            raise ValueError("server_ip must not be empty")
        if port < 1 or port > 65_535:
            raise ValueError("port must be between 1 and 65535")

        self.server_ip = server_ip
        self.port = port
        self._socket.connect(f"tcp://{server_ip}:{port}")
        logging.info("Connected attack interface to %s:%s", server_ip, port)

    def configure(
        self,
        label: str,
    ) -> None:
        self._require_open()
        encoded_label = label.encode("utf-8")
        if (
            not label.strip()
            or len(encoded_label) > 255
            or any(ord(character) < 0x20 or ord(character) == 0x7F for character in label)
        ):
            raise AttackInterfaceError(
                "label must contain 1-255 bytes of printable text"
            )
        self.session_label = label
        self._send(CfgDataMessage(label, 0, 0))
        self._configured = True
        self._next_heartbeat_at = self._monotonic() + self.HEARTBEAT_INTERVAL_SECONDS
        logging.info("Configured attack session '%s'", label)

    def tap_communication(
        self,
        channels: str | Sequence[str],
        turbine_ids: Sequence[int | bool],
    ) -> None:
        self._set_control(ControlSignal.CTRL_TAP, channels, turbine_ids)

    def fdi_communication(
        self,
        channels: str | Sequence[str],
        turbine_ids: Sequence[int | bool],
    ) -> None:
        self._set_control(ControlSignal.CTRL_FDI, channels, turbine_ids)

    def _set_control(
        self,
        signal: ControlSignal,
        channels: str | Sequence[str],
        turbine_ids: Sequence[int | bool],
    ) -> None:
        self._require_open()
        self._require_configured()
        names = [channels] if isinstance(channels, str) else list(channels)
        enabled = self._validate_turbine_flags(turbine_ids)
        target = self._tap_cfg if signal == ControlSignal.CTRL_TAP else self._fdi_cfg

        for name in names:
            try:
                data_type = self.SIGNAL_TYPES[name]
            except KeyError as error:
                raise AttackInterfaceError(f"unknown attack channel: {name}") from error
            target[name] = enabled.copy()
            self._send(CtDataMessage(signal, data_type, tuple(enabled)))
            logging.info(
                "Updated %s for %s: %s",
                "tap" if signal == ControlSignal.CTRL_TAP else "FDI",
                name,
                enabled,
            )

    def _validate_turbine_flags(
        self, turbine_ids: Sequence[int | bool]
    ) -> list[bool]:
        if isinstance(turbine_ids, (str, bytes)):
            raise AttackInterfaceError("turbine_ids must be a sequence of flags")
        enabled = [bool(value) for value in turbine_ids]
        if len(enabled) != self.num_turbines:
            raise AttackInterfaceError(
                f"turbine_ids must contain {self.num_turbines} values"
            )
        return enabled

    def begin(
        self,
        attack_func: AttackFunction | None = None,
    ) -> None:
        self._require_open()
        self._require_configured()
        if self.running:
            raise AttackInterfaceError("attack interface is already running")

        self._stop_event.clear()
        self._attack_func = attack_func
        self._running = True
        logging.info("Attack client started for session '%s'", self.session_label)
        if attack_func is not None:
            self._attack_thread = threading.Thread(
                target=self._attack_loop,
                name="attack-function",
                daemon=True,
            )
            self._attack_thread.start()

    def start(self, attack_func: AttackFunction) -> None:
        """Run the blocking attack workflow until stopped or interrupted."""
        self.begin(attack_func)
        try:
            self.run_forever()
        finally:
            self.stop()

    def run_forever(self) -> None:
        if not self.running:
            raise AttackInterfaceError("call begin() before run_forever()")

        while not self._stop_event.is_set():
            self.poll_once()
            self._stop_event.wait(self.INTERVAL_SECONDS)

    def poll_once(self) -> AttackMessage | None:
        self._require_open()
        self._send_heartbeat_if_due()
        try:
            raw = self._socket.recv(flags=zmq.NOBLOCK)
        except (BlockingIOError, zmq.Again):
            return None

        try:
            message = parse_message(raw)
        except (AttackProtocolError, ValueError) as error:
            logging.warning("Ignoring malformed attack-interface message: %s", error)
            return None

        self._handle_message(message)
        return message

    _execute = poll_once

    def _handle_message(self, message: object) -> None:
        if isinstance(message, TxDataMessage):
            self._handle_tx_data(message)
        elif isinstance(message, RqDataMessage):
            self._handle_rq_data(message)
        elif isinstance(message, (CfgDataMessage, HeartbeatMessage, ReleaseMessage)):
            return

    def _send_heartbeat_if_due(self) -> None:
        if not self._configured:
            return
        now = self._monotonic()
        if now < self._next_heartbeat_at:
            return
        self._send(HeartbeatMessage())
        self._publish_fdi_values()
        self._next_heartbeat_at = now + self.HEARTBEAT_INTERVAL_SECONDS

    def _publish_fdi_values(self) -> None:
        timestamp = self._wall_time_ms()
        for signal_name, enabled_turbines in self._fdi_cfg.items():
            signal_type = self.SIGNAL_TYPES[signal_name]
            for turbine_index, enabled in enumerate(enabled_turbines):
                value = self.fdi_next[signal_name][turbine_index]
                if enabled and not math.isnan(value):
                    self._send(AtDataMessage(turbine_index + 1, signal_type, timestamp, value))

    def _handle_tx_data(self, message: TxDataMessage) -> None:
        signal_name = self._TYPE2TEXT.get(message.data_type)
        if signal_name is None or not self._valid_turbine(message.turbine_id):
            return
        self.last_received[signal_name][message.turbine_id - 1] = message.value

    def _handle_rq_data(self, message: RqDataMessage) -> None:
        if self._wall_time_ms() > message.expiry_time_ms:
            return
        signal_name = self._TYPE2TEXT.get(message.data_type)
        if signal_name is None or not self._valid_turbine(message.turbine_id):
            return

        value = self.fdi_next[signal_name][message.turbine_id - 1]
        self._send(
            AtDataMessage(
                message.turbine_id,
                message.data_type,
                self._wall_time_ms(),
                value,
            )
        )

    def _attack_loop(self) -> None:
        started_at = self._monotonic()
        logging.info("Attack callback started for session '%s'", self.session_label)
        try:
            while not self._stop_event.is_set():
                if self._attack_func is not None:
                    elapsed_ms = int((self._monotonic() - started_at) * 1_000)
                    self._attack_func(self.last_received, self.fdi_next, elapsed_ms)
                self._stop_event.wait(self.ATTACK_INTERVAL_SECONDS)
        except Exception:
            logging.exception("Attack function failed; stopping attack client")
            self._stop_event.set()
        finally:
            logging.info("Attack callback stopped for session '%s'", self.session_label)

    def stop(self) -> None:
        self._stop_event.set()
        self._running = False

        thread = self._attack_thread
        if thread is not None and thread is not threading.current_thread():
            thread.join()
        self._attack_thread = None

        if self._closed:
            return
        try:
            self.release()
        except Exception:
            logging.warning("Could not release attack session cleanly", exc_info=True)
        self._closed = True
        try:
            try:
                self._socket.close(linger=0)
            except TypeError:
                self._socket.close()
        finally:
            if self._owns_context and self._context is not None:
                self._context.term()

    def release(self) -> None:
        """Release the configured attack session without closing the client."""
        if self._configured:
            self._send(ReleaseMessage())
            self._configured = False
            self._next_heartbeat_at = float("inf")
            logging.info("Released attack session '%s'", self.session_label)

    close = stop

    def _send(self, message: object) -> None:
        self._require_open()
        pack = getattr(message, "pack", None)
        if pack is None:
            raise TypeError("attack-interface messages must provide pack()")
        self._socket.send(pack())

    def _valid_turbine(self, turbine_id: int) -> bool:
        return 1 <= turbine_id <= self.num_turbines

    def _require_open(self) -> None:
        if self._closed:
            raise AttackInterfaceError("attack interface is closed")

    def _require_configured(self) -> None:
        if not self._configured:
            raise AttackInterfaceError("call configure() before changing or starting an attack")

    def __enter__(self) -> "AttackClient":
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc_value: BaseException | None,
        traceback: TracebackType | None,
    ) -> None:
        self.stop()


class AttackInterface(AttackClient):
    """Participant-facing attack-interface class."""
