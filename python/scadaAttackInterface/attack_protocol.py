"""Explicit attack-interface wire encoding and stream decoding."""

from __future__ import annotations

import math
import struct
from dataclasses import dataclass
from enum import IntEnum


class AttackProtocolError(ValueError):
    """Raised when an attack-interface message is invalid."""


class DataHeader(IntEnum):
    TX_DATA = 0x01
    RQ_DATA = 0x02
    AT_DATA = 0x04
    CT_DATA = 0x08
    CFG_DATA = 0x10
    SIM_CTRL = 0x20
    HEARTBEAT = 0x40
    RELEASE = 0x80


class TxDataType(IntEnum):
    TX_WS = 0x01
    TX_WD = 0x02
    TX_ST = 0x03
    TX_PW = 0x04
    TX_YAW = 0x05
    TX_RPM = 0x06
    TX_PTCH = 0x07
    TX_SPT_YAW = 0x08
    TX_SPT_PWR = 0x09
    TX_OP_CMD = 0x0A
    TX_GENTORQ = 0x10
    TX_GEN_TORQ = 0x10  # Compatibility spelling used by HackAWindFarm.
    TX_NONE = 0xFE
    TX_ARRAY = 0xFF


class ControlSignal(IntEnum):
    CTRL_NONE = 0x00
    CTRL_TAP = 0x01
    CTRL_FDI = 0x02


_TX_HEADER = struct.Struct("<BB2xIB3x")
_TX_FLOAT_VALUE = struct.Struct("<f")
_TX_INTEGER_VALUE = struct.Struct("<I")
_RQ = struct.Struct("<BB2xIQQ")
_AT = struct.Struct("<BB2xIQf4x")
_CT_HEADER = struct.Struct("<B3xII")
_CFG = struct.Struct("<B256s3xii")
_SIM_CONTROL = struct.Struct("<BB")
_SESSION_CONTROL = struct.Struct("<B")
DEFAULT_BUFFER_LIMIT = 64 * 1024
_INTEGER_TX_DATA_TYPES = {
    TxDataType.TX_ST,
    TxDataType.TX_OP_CMD,
}


def _require_size(data: bytes, expected: int, message_name: str) -> None:
    if len(data) != expected:
        raise AttackProtocolError(f"{message_name} requires {expected} bytes, received {len(data)}")


@dataclass(frozen=True)
class TxDataMessage:
    turbine_id: int
    data_type: TxDataType
    value: float | int

    def pack(self) -> bytes:
        header = _TX_HEADER.pack(
            DataHeader.TX_DATA,
            self.turbine_id,
            int(self.data_type),
            1,
        )
        if self.data_type in _INTEGER_TX_DATA_TYPES:
            if isinstance(self.value, bool) or not isinstance(self.value, int):
                raise AttackProtocolError("attack signal requires an integer value")
            if self.value < 0 or self.value > 0xFFFFFFFF:
                raise AttackProtocolError("attack integer value is outside uint32 range")
            return header + _TX_INTEGER_VALUE.pack(self.value)

        if isinstance(self.value, bool) or not isinstance(self.value, (float, int)):
            raise AttackProtocolError("attack signal requires a floating-point value")
        value = float(self.value)
        if not math.isfinite(value):
            raise AttackProtocolError("attack observation must be finite")
        return header + _TX_FLOAT_VALUE.pack(value)

    @classmethod
    def unpack(cls, data: bytes) -> "TxDataMessage":
        _require_size(data, _TX_HEADER.size + _TX_FLOAT_VALUE.size, cls.__name__)
        header, turbine_id, data_type, payload_length = _TX_HEADER.unpack_from(data)
        if header != DataHeader.TX_DATA:
            raise AttackProtocolError("invalid TX_DATA header")
        if payload_length != 1:
            raise AttackProtocolError("invalid TX_DATA payload length")

        typed_data = TxDataType(data_type)
        if typed_data in _INTEGER_TX_DATA_TYPES:
            value = _TX_INTEGER_VALUE.unpack_from(data, _TX_HEADER.size)[0]
        else:
            value = _TX_FLOAT_VALUE.unpack_from(data, _TX_HEADER.size)[0]
            if not math.isfinite(value):
                raise AttackProtocolError("attack observation must be finite")
        
        return cls(turbine_id, typed_data, value)


@dataclass(frozen=True)
class RqDataMessage:
    turbine_id: int
    data_type: TxDataType
    request_time_ms: int
    expiry_time_ms: int

    def pack(self) -> bytes:
        return _RQ.pack(DataHeader.RQ_DATA, self.turbine_id, int(self.data_type), self.request_time_ms, self.expiry_time_ms)

    @classmethod
    def unpack(cls, data: bytes) -> "RqDataMessage":
        _require_size(data, _RQ.size, cls.__name__)
        header, turbine_id, data_type, request_time, expiry_time = _RQ.unpack(data)
        if header != DataHeader.RQ_DATA:
            raise AttackProtocolError("invalid RQ_DATA header")
        
        return cls(turbine_id, TxDataType(data_type), request_time, expiry_time)


@dataclass(frozen=True)
class AtDataMessage:
    turbine_id: int
    data_type: TxDataType
    attack_time_ms: int
    fake_value: float

    def pack(self) -> bytes:
        if math.isinf(self.fake_value):
            raise AttackProtocolError("attack replacement must not be infinite")
        return _AT.pack(DataHeader.AT_DATA, self.turbine_id, int(self.data_type), self.attack_time_ms, self.fake_value)

    @classmethod
    def unpack(cls, data: bytes) -> "AtDataMessage":
        _require_size(data, _AT.size, cls.__name__)
        header, turbine_id, data_type, attack_time, fake_value = _AT.unpack(data)
        if header != DataHeader.AT_DATA:
            raise AttackProtocolError("invalid AT_DATA header")
        if math.isinf(fake_value):
            raise AttackProtocolError("attack replacement must not be infinite")
        
        return cls(turbine_id, TxDataType(data_type), attack_time, fake_value)


@dataclass(frozen=True)
class CtDataMessage:
    signal: ControlSignal
    data_type: TxDataType
    enable: tuple[bool, ...]

    def pack(self) -> bytes:
        header = _CT_HEADER.pack(DataHeader.CT_DATA, int(self.signal), int(self.data_type))
        return header + bytes(int(value) for value in self.enable)

    @classmethod
    def unpack(cls, data: bytes) -> "CtDataMessage":
        if len(data) < _CT_HEADER.size:
            raise AttackProtocolError(f"{cls.__name__} requires at least {_CT_HEADER.size} bytes")
        
        header, signal, data_type = _CT_HEADER.unpack_from(data)
        if header != DataHeader.CT_DATA:
            raise AttackProtocolError("invalid CT_DATA header")
        
        raw_enable = data[_CT_HEADER.size :]
        if any(value not in (0, 1) for value in raw_enable):
            # Do not interpret pointer bytes from the retired native layout as flags.
            raise AttackProtocolError("invalid CT_DATA enable flag")
        
        enable = tuple(bool(value) for value in raw_enable)
        return cls(ControlSignal(signal), TxDataType(data_type), enable)


@dataclass(frozen=True)
class CfgDataMessage:
    team_name: str
    scenario_id: int
    turbine_controller: int

    def pack(self) -> bytes:
        encoded_name = self.team_name.encode("utf-8")[:255]
        encoded_name += b"\0" * (256 - len(encoded_name))
        return _CFG.pack(DataHeader.CFG_DATA, encoded_name, self.scenario_id, self.turbine_controller)

    @classmethod
    def unpack(cls, data: bytes) -> "CfgDataMessage":
        _require_size(data, _CFG.size, cls.__name__)
        header, team_name, scenario_id, turbine_controller = _CFG.unpack(data)
        if header != DataHeader.CFG_DATA:
            raise AttackProtocolError("invalid CFG_DATA header")
        
        decoded_name = team_name.split(b"\0", 1)[0].decode("utf-8", errors="replace")
        return cls(decoded_name, scenario_id, turbine_controller)


@dataclass(frozen=True)
class SimCtrlMessage:
    sim_start: bool

    def pack(self) -> bytes:
        return _SIM_CONTROL.pack(DataHeader.SIM_CTRL, int(self.sim_start))

    @classmethod
    def unpack(cls, data: bytes) -> "SimCtrlMessage":
        _require_size(data, _SIM_CONTROL.size, cls.__name__)
        header, sim_start = _SIM_CONTROL.unpack(data)
        if header != DataHeader.SIM_CTRL:
            raise AttackProtocolError("invalid SIM_CTRL header")
        
        if sim_start not in (0, 1):
            raise AttackProtocolError("invalid SIM_CTRL flag")
        
        return cls(bool(sim_start))


@dataclass(frozen=True)
class HeartbeatMessage:
    def pack(self) -> bytes:
        return _SESSION_CONTROL.pack(DataHeader.HEARTBEAT)

    @classmethod
    def unpack(cls, data: bytes) -> "HeartbeatMessage":
        _require_size(data, _SESSION_CONTROL.size, cls.__name__)
        if _SESSION_CONTROL.unpack(data)[0] != DataHeader.HEARTBEAT:
            raise AttackProtocolError("invalid HEARTBEAT header")
        
        return cls()


@dataclass(frozen=True)
class ReleaseMessage:
    def pack(self) -> bytes:
        return _SESSION_CONTROL.pack(DataHeader.RELEASE)

    @classmethod
    def unpack(cls, data: bytes) -> "ReleaseMessage":
        _require_size(data, _SESSION_CONTROL.size, cls.__name__)
        if _SESSION_CONTROL.unpack(data)[0] != DataHeader.RELEASE:
            raise AttackProtocolError("invalid RELEASE header")
        
        return cls()


AttackMessage = (
    TxDataMessage
    | RqDataMessage
    | AtDataMessage
    | CtDataMessage
    | CfgDataMessage
    | SimCtrlMessage
    | HeartbeatMessage
    | ReleaseMessage
)


def parse_message(data: bytes) -> AttackMessage:
    if not data:
        raise AttackProtocolError("empty attack-interface message")

    try:
        header = DataHeader(data[0])

    except ValueError as error:
        raise AttackProtocolError(f"unknown message header 0x{data[0]:02x}") from error

    message_types = {
        DataHeader.TX_DATA: TxDataMessage,
        DataHeader.RQ_DATA: RqDataMessage,
        DataHeader.AT_DATA: AtDataMessage,
        DataHeader.CT_DATA: CtDataMessage,
        DataHeader.CFG_DATA: CfgDataMessage,
        DataHeader.SIM_CTRL: SimCtrlMessage,
        DataHeader.HEARTBEAT: HeartbeatMessage,
        DataHeader.RELEASE: ReleaseMessage,
    }

    return message_types[header].unpack(data)


def message_size(header: DataHeader | int, num_turbines: int) -> int:
    """Return the canonical byte size for a message header."""
    if num_turbines <= 0:
        raise ValueError("num_turbines must be positive")
    
    try:
        message_header = DataHeader(header)

    except ValueError as error:
        raise AttackProtocolError(f"unknown message header 0x{int(header):02x}") from error

    sizes = {
        DataHeader.TX_DATA: _TX_HEADER.size + _TX_FLOAT_VALUE.size,
        DataHeader.RQ_DATA: _RQ.size,
        DataHeader.AT_DATA: _AT.size,
        DataHeader.CT_DATA: _CT_HEADER.size + num_turbines,
        DataHeader.CFG_DATA: _CFG.size,
        DataHeader.SIM_CTRL: _SIM_CONTROL.size,
        DataHeader.HEARTBEAT: _SESSION_CONTROL.size,
        DataHeader.RELEASE: _SESSION_CONTROL.size,
    }
    return sizes[message_header]


class AttackStreamDecoder:
    """Split an attack TCP byte stream into validated messages."""

    def __init__(self, num_turbines: int, max_buffer_bytes: int = DEFAULT_BUFFER_LIMIT) -> None:
        if num_turbines <= 0:
            raise ValueError("num_turbines must be positive")
        
        if max_buffer_bytes <= 0:
            raise ValueError("max_buffer_bytes must be positive")
        
        self._num_turbines = num_turbines
        self._max_buffer_bytes = max_buffer_bytes
        self._buffer = bytearray()

    @property
    def buffered_bytes(self) -> int:
        return len(self._buffer)

    def feed(self, data: bytes) -> list[AttackMessage]:
        if len(data) > self._max_buffer_bytes - len(self._buffer):
            raise AttackProtocolError("attack receive buffer limit exceeded")
        
        self._buffer.extend(data)

        messages: list[AttackMessage] = []
        while self._buffer:
            expected_size = message_size(self._buffer[0], self._num_turbines)
            if len(self._buffer) < expected_size:
                break

            payload = bytes(self._buffer[:expected_size])
            del self._buffer[:expected_size]
            messages.append(parse_message(payload))
            
        return messages

    def finish(self) -> None:
        if self._buffer:
            raise AttackProtocolError("truncated attack message at end of stream")

    def reset(self) -> None:
        self._buffer.clear()
