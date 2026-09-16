# SPDX-License-Identifier: Apache-2.0
# Adapted from ivovs-tud/HackAWindFarm; substantially modified for packaging,
# shared protocol types, dependency injection, and deterministic lifecycle.

"""Legacy ZeroMQ attack client with a finite, testable lifecycle.

This module adapts the participant-facing API from the separately distributed
HackAWindFarm AttackInterface.py. Protocol encoding is provided by
legacy_attack_protocol rather than duplicated here. The ZeroMQ dependency is
optional until a real socket is requested.
"""

from __future__ import annotations

import logging
import threading
import time
from collections.abc import Callable, Sequence
from types import TracebackType
from typing import Any

from .legacy_attack_protocol import (
    AtDataMessage,
    CfgDataMessage,
    ControlSignal,
    CtDataMessage,
    LegacyMessage,
    LegacyProtocolError,
    RqDataMessage,
    SimCtrlMessage,
    TxDataMessage,
    TxDataType,
    parse_message,
)

try:
    import zmq as _zmq
except ModuleNotFoundError as error:
    if error.name != "zmq":
        raise
    _zmq = None


AttackFunction = Callable[[dict[str, list[float]], dict[str, list[float]], int], None]


class AttackInterfaceError(RuntimeError):
    """Base error raised by the integrated legacy attack client."""


class AttackClientDependencyError(AttackInterfaceError):
    """Raised when a real ZeroMQ client is requested without pyzmq."""


class LegacyAttackClient:
    """Client for the controller's legacy ZeroMQ attack interface."""

    INTERVAL_SECONDS = 0.01
    ATTACK_INTERVAL_SECONDS = 0.1
    ZEROMQ_MAX_MESSAGES = 10
    RECV_TIMEOUT_MS = 500
    SEND_TIMEOUT_MS = 500

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
        self.team_name = ""
        self.scenario_id = 0
        self.turbine_controller = 0

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

        self._tap_cfg = self._empty_control_map()
        self._fdi_cfg = self._empty_control_map()
        self.last_received = self._empty_value_map()
        self.fdi_next = self._empty_value_map()

    def _create_zmq_socket(self, context: Any | None) -> Any:
        if _zmq is None:
            raise AttackClientDependencyError(
                "pyzmq is required for network use; install supervisory-controller-client[attack]"
            )

        if context is None:
            context = _zmq.Context()
            self._owns_context = True
        self._context = context
        socket = context.socket(_zmq.PAIR)
        socket.setsockopt(_zmq.SNDHWM, self.ZEROMQ_MAX_MESSAGES)
        socket.setsockopt(_zmq.RCVTIMEO, self.RECV_TIMEOUT_MS)
        socket.setsockopt(_zmq.SNDTIMEO, self.SEND_TIMEOUT_MS)
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
        team_name: str,
        scenario_id: int = 0,
        turbine_controller: int = 0,
    ) -> None:
        self._require_open()
        self.team_name = team_name
        self.scenario_id = scenario_id
        self.turbine_controller = turbine_controller
        self._send(
            CfgDataMessage(team_name, scenario_id, turbine_controller)
        )

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
        *,
        wait_for_ready: bool = True,
        ready_timeout: float | None = None,
    ) -> None:
        self._require_open()
        if self.running:
            raise AttackInterfaceError("attack interface is already running")

        self._stop_event.clear()
        self._attack_func = attack_func
        self._send(SimCtrlMessage(True))
        if wait_for_ready:
            self.wait_for_ready(ready_timeout)

        self._running = True
        if attack_func is not None:
            self._attack_thread = threading.Thread(
                target=self._attack_loop,
                name="legacy-attack-function",
                daemon=True,
            )
            self._attack_thread.start()

    def start(self, attack_func: AttackFunction) -> None:
        """Run the legacy blocking workflow until stopped or interrupted."""
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

    def poll_once(self) -> LegacyMessage | None:
        self._require_open()
        try:
            raw = self._socket.recv(flags=self._nonblocking_flag())
        except Exception as error:
            if self._is_would_block(error):
                return None
            raise

        try:
            message = parse_message(raw)
        except (LegacyProtocolError, ValueError) as error:
            logging.warning("Ignoring malformed attack-interface message: %s", error)
            return None

        self._handle_message(message)
        return message

    _execute = poll_once

    def wait_for_ready(self, timeout: float | None = None) -> SimCtrlMessage:
        deadline = None if timeout is None else self._monotonic() + timeout
        while not self._stop_event.is_set():
            message = self.poll_once()
            if isinstance(message, SimCtrlMessage) and message.sim_start:
                return message
            if deadline is not None and self._monotonic() >= deadline:
                raise TimeoutError("timed out waiting for supervisory controller readiness")
            self._stop_event.wait(self.INTERVAL_SECONDS)
        raise AttackInterfaceError("attack interface stopped while waiting for readiness")

    def _handle_message(self, message: object) -> None:
        if isinstance(message, TxDataMessage):
            self._handle_tx_data(message)
        elif isinstance(message, RqDataMessage):
            self._handle_rq_data(message)
        elif isinstance(message, (SimCtrlMessage, CfgDataMessage)):
            return

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
        while not self._stop_event.is_set():
            try:
                if self._attack_func is not None:
                    elapsed_ms = int((self._monotonic() - started_at) * 1_000)
                    self._attack_func(self.last_received, self.fdi_next, elapsed_ms)
            except Exception:
                logging.exception("Attack function failed; stopping attack client")
                self._stop_event.set()
                return
            self._stop_event.wait(self.ATTACK_INTERVAL_SECONDS)

    def stop(self) -> None:
        self._stop_event.set()
        self._running = False

        thread = self._attack_thread
        if thread is not None and thread is not threading.current_thread():
            thread.join()
        self._attack_thread = None

        if self._closed:
            return
        self._closed = True
        try:
            try:
                self._socket.close(linger=0)
            except TypeError:
                self._socket.close()
        finally:
            if self._owns_context and self._context is not None:
                self._context.term()

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

    @staticmethod
    def _nonblocking_flag() -> int:
        return 1 if _zmq is None else int(_zmq.NOBLOCK)

    @staticmethod
    def _is_would_block(error: Exception) -> bool:
        if isinstance(error, BlockingIOError):
            return True
        return _zmq is not None and isinstance(error, _zmq.Again)

    def __enter__(self) -> "LegacyAttackClient":
        return self

    def __exit__(
        self,
        exc_type: type[BaseException] | None,
        exc_value: BaseException | None,
        traceback: TracebackType | None,
    ) -> None:
        self.stop()


class AttackInterface(LegacyAttackClient):
    """Backward-compatible participant-facing class name."""
