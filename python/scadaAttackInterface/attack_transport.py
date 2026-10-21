"""Transport adapters for the attack-interface protocol."""

from __future__ import annotations

import selectors
import socket as socket_module
import time
from abc import ABC, abstractmethod
from collections import deque
from typing import Any

import zmq

from .attack_protocol import (
    AttackMessage,
    AttackProtocolError,
    AttackStreamDecoder,
    DEFAULT_BUFFER_LIMIT,
    DEFAULT_HEARTBEAT_INTERVAL_MS,
    DEFAULT_HEARTBEAT_TIMEOUT_MS,
    parse_message,
)


class AttackTransportError(ConnectionError):
    """Raised when an attack transport cannot make progress."""


class AttackTransport(ABC):
    """Byte transport used by the attack client."""

    @abstractmethod
    def connect(self, server_ip: str, port: int) -> None:
        pass

    @abstractmethod
    def send(self, payload: bytes) -> None:
        pass

    @abstractmethod
    def receive(self, timeout: float = 0.0) -> list[AttackMessage]:
        pass

    @abstractmethod
    def flush(self, timeout: float) -> bool:
        pass

    @abstractmethod
    def close(self) -> None:
        pass


class ZeroMqAttackTransport(AttackTransport):
    """ZeroMQ PAIR adapter retained for compatibility."""

    MAX_MESSAGES = 10
    RECEIVE_TIMEOUT_MS = 500
    SEND_TIMEOUT_MS = 500
    HEARTBEAT_INTERVAL_MS = DEFAULT_HEARTBEAT_INTERVAL_MS
    HEARTBEAT_TIMEOUT_MS = DEFAULT_HEARTBEAT_TIMEOUT_MS

    def __init__(self, *, socket: Any | None = None, context: Any | None = None) -> None:
        self._context = context
        self._owns_context = False
        self._socket = socket
        if self._socket is None:
            if self._context is None:
                self._context = zmq.Context()
                self._owns_context = True

            self._socket = self._context.socket(zmq.PAIR)
            self._socket.setsockopt(zmq.SNDHWM, self.MAX_MESSAGES)
            self._socket.setsockopt(zmq.RCVTIMEO, self.RECEIVE_TIMEOUT_MS)
            self._socket.setsockopt(zmq.SNDTIMEO, self.SEND_TIMEOUT_MS)
            self._socket.setsockopt(zmq.HEARTBEAT_IVL, self.HEARTBEAT_INTERVAL_MS)
            self._socket.setsockopt(zmq.HEARTBEAT_TIMEOUT, self.HEARTBEAT_TIMEOUT_MS)

        self._closed = False

    def connect(self, server_ip: str, port: int) -> None:
        try:
            self._socket.connect(f"tcp://{server_ip}:{port}")

        except (OSError, zmq.ZMQError) as error:
            raise AttackTransportError(f"ZeroMQ connection failed: {error}") from error

    def send(self, payload: bytes) -> None:
        if self._closed:
            raise AttackTransportError("ZeroMQ transport is closed")
        
        try:
            self._socket.send(payload)

        except (OSError, zmq.ZMQError) as error:
            raise AttackTransportError(f"ZeroMQ send failed: {error}") from error

    def receive(self, timeout: float = 0.0) -> list[AttackMessage]:
        if self._closed:
            raise AttackTransportError("ZeroMQ transport is closed")
        
        try:
            poll = getattr(self._socket, "poll", None)
            if poll is not None and not poll(max(0, int(timeout * 1000)), zmq.POLLIN):
                return []
            
            raw = self._socket.recv(flags=zmq.NOBLOCK)
            return [parse_message(bytes(raw))]
        
        except (BlockingIOError, zmq.Again):
            if timeout > 0 and getattr(self._socket, "poll", None) is None:
                time.sleep(timeout)

            return []
        
        except AttackProtocolError:
            raise

        except (OSError, zmq.ZMQError) as error:
            raise AttackTransportError(f"ZeroMQ receive failed: {error}") from error

    def flush(self, timeout: float) -> bool:
        del timeout
        return not self._closed

    def close(self) -> None:
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


class TcpAttackTransport(AttackTransport):
    """Non-blocking TCP adapter with bounded FIFO writes and stream decoding."""

    CONNECT_TIMEOUT_SECONDS = 1.0

    def __init__(self, num_turbines: int, *, receive_buffer_bytes: int = DEFAULT_BUFFER_LIMIT, transmit_buffer_bytes: int = DEFAULT_BUFFER_LIMIT) -> None:
        if receive_buffer_bytes <= 0 or transmit_buffer_bytes <= 0:
            raise ValueError("TCP attack buffers must be positive")
        self._decoder = AttackStreamDecoder(num_turbines, receive_buffer_bytes)
        self._receive_buffer_bytes = receive_buffer_bytes
        self._transmit_buffer_bytes = transmit_buffer_bytes
        self._socket: socket_module.socket | None = None
        self._selector: selectors.BaseSelector | None = None
        self._outbound: deque[bytes] = deque()
        self._send_offset = 0
        self._queued_bytes = 0
        self._received: deque[AttackMessage] = deque()

    def connect(self, server_ip: str, port: int) -> None:
        if self._socket is not None:
            raise AttackTransportError("TCP transport is already connected")
        
        connection: socket_module.socket | None = None
        selector: selectors.BaseSelector | None = None
        try:
            connection = socket_module.create_connection((server_ip, port), timeout=self.CONNECT_TIMEOUT_SECONDS)
            connection.setsockopt(socket_module.IPPROTO_TCP, socket_module.TCP_NODELAY, 1)
            connection.setsockopt(socket_module.SOL_SOCKET, socket_module.SO_KEEPALIVE, 1)
            connection.setblocking(False)
            selector = selectors.DefaultSelector()
            selector.register(connection, selectors.EVENT_READ)

        except OSError as error:
            if selector is not None:
                selector.close()

            if connection is not None:
                connection.close()

            raise AttackTransportError(f"TCP connection failed: {error}") from error
        
        self._socket = connection
        self._selector = selector

    def send(self, payload: bytes) -> None:
        self._require_connected()
        if not payload:
            raise AttackTransportError("cannot send an empty attack message")
        
        if len(payload) > self._transmit_buffer_bytes - self._queued_bytes:
            raise AttackTransportError("TCP attack transmit buffer overflow")
        
        self._outbound.append(bytes(payload))
        self._queued_bytes += len(payload)
        self._update_selector()

    def receive(self, timeout: float = 0.0) -> list[AttackMessage]:
        self._require_connected()
        if not self._received:
            self._service(timeout)

        messages = list(self._received)
        self._received.clear()
        return messages

    def flush(self, timeout: float) -> bool:
        self._require_connected()
        deadline = time.monotonic() + max(0.0, timeout)
        while self._outbound:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return False
            
            self._service(remaining)

        return True

    def close(self) -> None:
        selector = self._selector
        connection = self._socket
        self._selector = None
        self._socket = None
        if selector is not None:
            try:
                if connection is not None:
                    selector.unregister(connection)

            except (KeyError, ValueError):
                pass

            selector.close()

        if connection is not None:
            connection.close()

        self._outbound.clear()
        self._received.clear()
        self._send_offset = 0
        self._queued_bytes = 0
        self._decoder.reset()

    def _service(self, timeout: float) -> None:
        self._require_connected()
        assert self._selector is not None
        try:
            events = self._selector.select(max(0.0, timeout))
            for _, mask in events:
                if mask & selectors.EVENT_WRITE:
                    self._write_available()

                if mask & selectors.EVENT_READ:
                    self._read_available()

        except AttackProtocolError:
            self.close()
            raise

        except (ConnectionError, OSError) as error:
            self.close()
            raise AttackTransportError(f"TCP connection failed: {error}") from error

    def _read_available(self) -> None:
        assert self._socket is not None
        while True:
            try:
                data = self._socket.recv(4096)

            except BlockingIOError:
                return
            
            if not data:
                try:
                    self._decoder.finish()

                except AttackProtocolError as error:
                    raise AttackTransportError(str(error)) from error
                
                raise AttackTransportError("TCP attack server closed the connection")

            offset = 0
            while offset < len(data):
                available = self._receive_buffer_bytes - self._decoder.buffered_bytes
                if available == 0:
                    raise AttackProtocolError("attack receive buffer limit exceeded")
                
                chunk_size = min(available, len(data) - offset)
                self._received.extend(self._decoder.feed(data[offset : offset + chunk_size]))
                offset += chunk_size

    def _write_available(self) -> None:
        assert self._socket is not None
        while self._outbound:
            message = self._outbound[0]
            try:
                sent = self._socket.send(message[self._send_offset :])

            except BlockingIOError:
                return
            
            if sent == 0:
                raise AttackTransportError("TCP attack send returned zero")
            
            self._send_offset += sent
            self._queued_bytes -= sent
            if self._send_offset == len(message):
                self._outbound.popleft()
                self._send_offset = 0

        self._update_selector()

    def _update_selector(self) -> None:
        self._require_connected()
        assert self._selector is not None
        assert self._socket is not None
        events = selectors.EVENT_READ
        if self._outbound:
            events |= selectors.EVENT_WRITE
            
        self._selector.modify(self._socket, events)

    def _require_connected(self) -> None:
        if self._socket is None or self._selector is None:
            raise AttackTransportError("TCP transport is not connected")
