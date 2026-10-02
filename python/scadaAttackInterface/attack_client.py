# SPDX-License-Identifier: GPL-3.0-or-later

"""Transport-neutral attack client with a finite, testable lifecycle."""

from __future__ import annotations

import logging
import threading
import time
from collections import deque
from collections.abc import Callable, Sequence
from dataclasses import dataclass, field
from types import TracebackType
from typing import Any

from .attack_protocol import (AtDataMessage, AttackMessage, AttackProtocolError, CfgDataMessage, ControlSignal,
                                CtDataMessage, DEFAULT_HEARTBEAT_INTERVAL_MS, HeartbeatMessage, ReleaseMessage,
                                RqDataMessage, TxDataMessage, TxDataType)

from .attack_transport import AttackTransport, AttackTransportError, TcpAttackTransport, ZeroMqAttackTransport


AttackFunction = Callable[[dict[str, list[float]], dict[str, list[float]], int], None]


class AttackInterfaceError(RuntimeError):
    """Base error raised by the attack client."""


@dataclass
class _OutboundItem:
    payload: bytes
    completion: threading.Event | None = None
    error: BaseException | None = field(default=None, init=False)


class AttackClient:
    """Client for either supported controller attack transport."""

    INTERVAL_SECONDS = 0.01
    ATTACK_INTERVAL_SECONDS = 0.1
    CONFIGURATION_TIMEOUT_SECONDS = 1.0
    SEND_COMPLETION_TIMEOUT_SECONDS = 1.0
    OUTBOUND_BUFFER_BYTES = 64 * 1024
    MAX_RECEIVED_EVENTS = 256

    ZEROMQ_MAX_MESSAGES = ZeroMqAttackTransport.MAX_MESSAGES
    RECV_TIMEOUT_MS = ZeroMqAttackTransport.RECEIVE_TIMEOUT_MS
    SEND_TIMEOUT_MS = ZeroMqAttackTransport.SEND_TIMEOUT_MS
    HEARTBEAT_INTERVAL_SECONDS = DEFAULT_HEARTBEAT_INTERVAL_MS / 1_000
    ZEROMQ_HEARTBEAT_INTERVAL_MS = ZeroMqAttackTransport.HEARTBEAT_INTERVAL_MS
    ZEROMQ_HEARTBEAT_TIMEOUT_MS = ZeroMqAttackTransport.HEARTBEAT_TIMEOUT_MS

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
    _NORMALIZED_SIGNAL_NAMES: dict[str, str] = {}
    for _signal_name in SIGNAL_TYPES:
        _NORMALIZED_SIGNAL_NAMES["".join(character for character in _signal_name.casefold() if character.isalnum())] = _signal_name

    del _signal_name
    _AttackInterfaceExcept = AttackInterfaceError

    def __init__( self, num_turbines: int = 9, *, transport: str | AttackTransport = "zeromq", 
                 socket: Any | None = None, context: Any | None = None, 
                 wall_time_ms: Callable[[], int] | None = None, monotonic: Callable[[], float] | None = None) -> None:
        
        if num_turbines <= 0:
            raise ValueError("num_turbines must be positive")
        
        if socket is not None and not isinstance(transport, str):
            raise ValueError("socket injection cannot be combined with a custom transport")

        self.num_turbines = num_turbines
        self.server_ip: str | None = None
        self.port: int | None = None
        self.session_label = ""

        self._transport_factory: Callable[[], AttackTransport] | None
        if isinstance(transport, str):
            transport_name = transport.lower()
            if transport_name == "zeromq":
                self._transport_factory = lambda: ZeroMqAttackTransport(context=context)
                self._transport = ZeroMqAttackTransport(socket=socket, context=context)
                if socket is not None:
                    self._transport_factory = None

            elif transport_name == "tcp":
                if socket is not None or context is not None:
                    raise ValueError("socket and context injection are only supported for ZeroMQ")
                
                self._transport_factory = lambda: TcpAttackTransport(self.num_turbines)
                self._transport = self._transport_factory()
            else:
                raise ValueError("transport must be 'zeromq', 'tcp', or an AttackTransport")
            
            self.transport = transport_name
        else:
            if context is not None:
                raise ValueError("context injection cannot be combined with a custom transport")
            
            self._transport_factory = None
            self._transport = transport
            self.transport = transport.__class__.__name__

        self._wall_time_ms = wall_time_ms or (lambda: time.time_ns() // 1_000_000)
        self._monotonic = monotonic or time.monotonic
        self._stop_event = threading.Event()
        self._io_stop_event = threading.Event()
        self._attack_thread: threading.Thread | None = None
        self._io_thread: threading.Thread | None = None
        self._attack_func: AttackFunction | None = None
        self._running = False
        self._closed = False
        self._connected = False
        self._configured = False
        self._next_heartbeat_at = float("inf")
        self._io_error: AttackInterfaceError | None = None

        self._outbound_lock = threading.Lock()
        self._outbound: deque[_OutboundItem] = deque()
        self._outbound_bytes = 0
        self._received_lock = threading.Lock()
        self._received_events: deque[AttackMessage] = deque(maxlen=self.MAX_RECEIVED_EVENTS)

        self._tap_cfg = self._empty_control_map()
        self._fdi_cfg = self._empty_control_map()
        self.last_received = self._empty_value_map()
        self.fdi_next = self._empty_value_map()

    def _empty_control_map(self) -> dict[str, list[bool]]:
        return {name: [False] * self.num_turbines for name in self.SIGNAL_TYPES}

    def _empty_value_map(self) -> dict[str, list[float | int]]:
        return {name: [float("nan")] * self.num_turbines for name in self.SIGNAL_TYPES}

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
        
        if self._connected:
            raise AttackInterfaceError("attack interface is already connected")

        try:
            self._transport.connect(server_ip, port)

        except AttackTransportError as error:
            raise AttackInterfaceError(str(error)) from error
        
        self.server_ip = server_ip
        self.port = port
        self._connected = True
        logging.info("Connected %s attack interface to %s:%s", self.transport, server_ip, port)

    def reconnect(self, server_ip: str | None = None, port: int | None = None) -> None:
        if self.running:
            raise AttackInterfaceError("stop the attack client before reconnecting")
        
        if self._transport_factory is None:
            raise AttackInterfaceError("the injected transport cannot be recreated")
        
        target_ip = server_ip or self.server_ip
        target_port = port or self.port
        if target_ip is None or target_port is None:
            raise AttackInterfaceError("connect once or provide a reconnect endpoint")

        self._stop_event.set()
        self._io_stop_event.set()
        for thread in (self._attack_thread, self._io_thread):
            if thread is not None and thread is not threading.current_thread():
                thread.join()

        self._attack_thread = None
        self._io_thread = None
        self._running = False
        self._transport.close()
        self._transport = self._transport_factory()
        self._closed = False
        self._connected = False
        self._end_local_session()
        self.session_label = ""
        self._stop_event.clear()
        self._io_stop_event.clear()
        self._io_error = None
        self.last_received = self._empty_value_map()
        self._clear_queues()
        self.connect(target_ip, target_port)

    def configure(self, label: str) -> None:
        self._require_open()
        if self._io_thread is not None and self._io_thread.is_alive():
            raise AttackInterfaceError("configure the session before begin()")
        
        encoded_label = label.encode("utf-8")
        if (not label.strip() or len(encoded_label) > 255 or any(ord(character) < 0x20 or ord(character) == 0x7F for character in label)):
            raise AttackInterfaceError("label must contain 1-255 bytes of printable text")

        configuration = CfgDataMessage(label, 0, 0)
        try:
            self._transport.send(configuration.pack())
            if not self._transport.flush(self.SEND_COMPLETION_TIMEOUT_SECONDS):
                raise AttackTransportError("configuration send timed out")
            
            self._wait_for_configuration_acknowledgement(configuration)

        except (AttackProtocolError, AttackTransportError) as error:
            self._end_local_session()
            raise AttackInterfaceError("attack server is unavailable or may already have an active client: " f"{error}") from error

        self._end_local_session()
        self.session_label = label
        self._configured = True
        self._next_heartbeat_at = self._monotonic() + self.HEARTBEAT_INTERVAL_SECONDS
        logging.info("Configured attack session '%s'", label)

    def _wait_for_configuration_acknowledgement(self, expected: CfgDataMessage) -> None:
        deadline = time.monotonic() + self.CONFIGURATION_TIMEOUT_SECONDS
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AttackTransportError("configuration acknowledgement timed out")
            
            for message in self._transport.receive(min(remaining, 0.05)):
                if message == expected:
                    return
                
                self._handle_message(message)

    def tap_communication(self, channels: str | Sequence[str], turbine_ids: Sequence[int | bool]) -> None:
        self._set_control(ControlSignal.CTRL_TAP, channels, turbine_ids)

    def fdi_communication(self, channels: str | Sequence[str], turbine_ids: Sequence[int | bool]) -> None:
        self._set_control(ControlSignal.CTRL_FDI, channels, turbine_ids)

    def _set_control(self, signal: ControlSignal, channels: str | Sequence[str], turbine_ids: Sequence[int | bool]) -> None:
        self._require_open()
        self._require_configured()
        names = [channels] if isinstance(channels, str) else list(channels)
        enabled = self._validate_turbine_flags(turbine_ids)
        target = self._tap_cfg if signal == ControlSignal.CTRL_TAP else self._fdi_cfg

        for name in names:
            canonical_name = self._canonical_signal_name(name)
            data_type = self.SIGNAL_TYPES[canonical_name]
            target[canonical_name] = enabled.copy()
            self._send(CtDataMessage(signal, data_type, tuple(enabled)))
            logging.info("Updated %s for %s: %s", "tap" if signal == ControlSignal.CTRL_TAP else "FDI", canonical_name, enabled)

    @classmethod
    def _canonical_signal_name(cls, name: object) -> str:
        if not isinstance(name, str):
            raise AttackInterfaceError(f"unknown attack channel: {name!r}")
        normalized = "".join(character for character in name.casefold() if character.isalnum())
        try:
            return cls._NORMALIZED_SIGNAL_NAMES[normalized]
        
        except KeyError as error:
            raise AttackInterfaceError(f"unknown attack channel: {name}") from error

    def _validate_turbine_flags(self, turbine_ids: Sequence[int | bool]) -> list[bool]:
        if isinstance(turbine_ids, (str, bytes)):
            raise AttackInterfaceError("turbine_ids must be a sequence of flags")
        
        enabled = [bool(value) for value in turbine_ids]
        if len(enabled) != self.num_turbines:
            raise AttackInterfaceError(f"turbine_ids must contain {self.num_turbines} values")
        
        return enabled

    def begin(self, attack_func: AttackFunction | None = None) -> None:
        self._require_open()
        self._require_configured()
        if self.running:
            raise AttackInterfaceError("attack interface is already running")

        self._stop_event.clear()
        self._io_stop_event.clear()
        self._io_error = None
        self._attack_func = attack_func
        self._running = True
        self._io_thread = threading.Thread(target=self._io_loop, name="attack-transport", daemon=True)
        self._io_thread.start()
        logging.info("Attack client started for session '%s'", self.session_label)
        if attack_func is not None:
            self._attack_thread = threading.Thread(target=self._attack_loop, name="attack-function", daemon=True)
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
        
        while not self._stop_event.wait(self.INTERVAL_SECONDS):
            self.poll_once()

        self._raise_io_error()

    def poll_once(self) -> AttackMessage | None:
        self._require_open()
        self._raise_io_error()
        io_thread = self._io_thread
        if io_thread is not None and io_thread.is_alive():
            return self._pop_received_event()

        self._send_heartbeat_if_due()
        try:
            messages = self._transport.receive(0.0)

        except AttackProtocolError as error:
            logging.warning("Ignoring malformed attack-interface message: %s", error)
            return None
        
        except AttackTransportError as error:
            failure = AttackInterfaceError(str(error))
            self._signal_failure(failure)
            self._transport.close()
            self._connected = False
            raise failure from error
        
        for message in messages:
            self._handle_message(message)
            self._record_received_event(message)

        return messages[0] if messages else None

    _execute = poll_once

    def _io_loop(self) -> None:
        failed = False
        transport_closed = False
        try:
            while not self._io_stop_event.is_set():
                self._send_heartbeat_if_due()
                self._drain_outbound()
                try:
                    messages = self._transport.receive(self.INTERVAL_SECONDS)

                except AttackProtocolError as error:
                    logging.warning("Ignoring malformed attack-interface message: %s", error)
                    continue

                for message in messages:
                    self._handle_message(message)
                    self._record_received_event(message)
                self._drain_outbound()

        except AttackTransportError as error:
            transport_closed = True
            if self._configured:
                failed = True
                self._signal_failure(AttackInterfaceError(str(error)))
                logging.error("Attack transport stopped: %s", error)

            else:
                logging.info("Attack transport closed after session release")

            self._stop_event.set()

        except (AttackProtocolError, AttackInterfaceError) as error:
            failed = True
            self._signal_failure(AttackInterfaceError(str(error)))
            logging.error("Attack transport stopped: %s", error)

        finally:
            failed = failed or self._io_error is not None
            if not failed and not transport_closed:
                try:
                    self._drain_outbound()
                    if not self._transport.flush(self.SEND_COMPLETION_TIMEOUT_SECONDS):
                        self._signal_failure(AttackInterfaceError("attack transport flush timed out"))
                        failed = True

                except (AttackTransportError, AttackInterfaceError) as error:
                    self._signal_failure(AttackInterfaceError(str(error)))
                    failed = True

            if failed or transport_closed:
                self._transport.close()
                self._connected = False
            self._running = False

    def _handle_message(self, message: object) -> None:
        if isinstance(message, TxDataMessage):
            self._handle_tx_data(message)

        elif isinstance(message, RqDataMessage):
            self._handle_rq_data(message)

    def _send_heartbeat_if_due(self) -> None:
        if not self._configured:
            return
        
        now = self._monotonic()
        if now < self._next_heartbeat_at:
            return
        
        self._send(HeartbeatMessage())
        self._next_heartbeat_at = now + self.HEARTBEAT_INTERVAL_SECONDS

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
        self._send(AtDataMessage(
            message.turbine_id,
            message.data_type,
            message.request_time_ms,
            value))

    def _attack_loop(self) -> None:
        started_at = self._monotonic()
        logging.info("Attack callback started for session '%s'", self.session_label)
        try:
            while not self._stop_event.is_set():
                if self._attack_func is not None:
                    elapsed_ms = int((self._monotonic() - started_at) * 1_000)
                    self._attack_func(self.last_received, self.fdi_next, elapsed_ms)
                self._stop_event.wait(self.ATTACK_INTERVAL_SECONDS)

        except Exception as error:
            logging.exception("Attack function failed; stopping attack client")
            self._signal_failure(AttackInterfaceError(f"attack function failed: {error}"))

        finally:
            logging.info("Attack callback stopped for session '%s'", self.session_label)

    def stop(self) -> None:
        if self._closed:
            return
        
        self._stop_event.set()

        attack_thread = self._attack_thread
        if attack_thread is not None and attack_thread is not threading.current_thread():
            attack_thread.join()

        self._attack_thread = None

        try:
            self.release()
        except Exception:
            logging.warning("Could not release attack session cleanly", exc_info=True)

        self._io_stop_event.set()
        io_thread = self._io_thread
        if io_thread is not None and io_thread is not threading.current_thread():
            io_thread.join()
        self._io_thread = None
        self._running = False
        self._closed = True
        self._connected = False
        self._transport.close()

    def release(self) -> None:
        """Release the configured attack session without closing the client."""
        if not self._configured:
            return
        
        self._end_local_session()
        self._send(ReleaseMessage(), wait=True)
        logging.info("Released attack session '%s'", self.session_label)

    close = stop

    def _send(self, message: object, *, wait: bool = False) -> None:
        self._require_open()
        pack = getattr(message, "pack", None)
        if pack is None:
            raise TypeError("attack-interface messages must provide pack()")
        payload = bytes(pack())
        io_thread = self._io_thread
        if threading.current_thread() is io_thread:
            try:
                self._transport.send(payload)
                if wait and not self._transport.flush(self.SEND_COMPLETION_TIMEOUT_SECONDS):
                    raise AttackTransportError("attack message send timed out")
                
            except AttackTransportError as error:
                raise AttackInterfaceError(str(error)) from error
            
            return
        
        if io_thread is not None and io_thread.is_alive():
            self._enqueue_payload(payload, wait=wait)
            return
        
        try:
            self._transport.send(payload)
            if wait and not self._transport.flush(self.SEND_COMPLETION_TIMEOUT_SECONDS):
                raise AttackTransportError("attack message send timed out")
            
        except AttackTransportError as error:
            raise AttackInterfaceError(str(error)) from error

    def _enqueue_payload(self, payload: bytes, *, wait: bool = False) -> None:
        completion = threading.Event() if wait else None
        item = _OutboundItem(payload, completion)
        overflow = False
        with self._outbound_lock:
            if len(payload) > self.OUTBOUND_BUFFER_BYTES - self._outbound_bytes:
                overflow = True
            else:
                self._outbound.append(item)
                self._outbound_bytes += len(payload)

        if overflow:
            failure = AttackInterfaceError("attack client outbound buffer overflow")
            self._signal_failure(failure)
            raise failure
        
        if completion is None:
            return
        
        if not completion.wait(self.SEND_COMPLETION_TIMEOUT_SECONDS):
            raise AttackInterfaceError("attack client send timed out")
        
        if item.error is not None:
            raise AttackInterfaceError(str(item.error)) from item.error

    def _drain_outbound(self) -> None:
        while True:
            with self._outbound_lock:
                if not self._outbound:
                    return
                item = self._outbound.popleft()
                self._outbound_bytes -= len(item.payload)

            try:
                self._transport.send(item.payload)
                if item.completion is not None and not self._transport.flush(self.SEND_COMPLETION_TIMEOUT_SECONDS):
                    raise AttackTransportError("attack message send timed out")
                
            except Exception as error:
                item.error = error
                if item.completion is not None:
                    item.completion.set()
                raise

            if item.completion is not None:
                item.completion.set()

    def _record_received_event(self, message: AttackMessage) -> None:
        with self._received_lock:
            self._received_events.append(message)

    def _pop_received_event(self) -> AttackMessage | None:
        with self._received_lock:
            return self._received_events.popleft() if self._received_events else None

    def _clear_queues(self) -> None:
        with self._outbound_lock:
            self._outbound.clear()
            self._outbound_bytes = 0
        with self._received_lock:
            self._received_events.clear()

    def _end_local_session(self) -> bool:
        was_configured = self._configured
        self._configured = False
        self._next_heartbeat_at = float("inf")
        self._tap_cfg = self._empty_control_map()
        self._fdi_cfg = self._empty_control_map()
        self.fdi_next = self._empty_value_map()
        return was_configured

    def _signal_failure(self, error: AttackInterfaceError) -> None:
        if self._io_error is None:
            self._io_error = error
        self._end_local_session()
        self._stop_event.set()
        self._io_stop_event.set()
        with self._outbound_lock:
            pending = list(self._outbound)
            self._outbound.clear()
            self._outbound_bytes = 0
        for item in pending:
            item.error = self._io_error
            if item.completion is not None:
                item.completion.set()

    def _raise_io_error(self) -> None:
        if self._io_error is not None:
            raise self._io_error

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

    def __exit__(self, exc_type: type[BaseException] | None, exc_value: BaseException | None, traceback: TracebackType | None) -> None:
        self.stop()


class AttackInterface(AttackClient):
    """Participant-facing attack-interface class."""
