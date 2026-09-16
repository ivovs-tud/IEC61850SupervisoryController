"""Legacy ZeroMQ attack-interface wire messages.

This module captures the x86-64 GCC-compatible native C++ layouts used by the
existing controller and HackAWindFarm client. It is a compatibility layer, not
the protocol intended for the future framed stream transport.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass
from enum import IntEnum


class LegacyProtocolError(ValueError):
    """Raised when a legacy attack-interface message is invalid."""


class DataHeader(IntEnum):
    TX_DATA = 0x01
    RQ_DATA = 0x02
    AT_DATA = 0x04
    CT_DATA = 0x08
    CFG_DATA = 0x10
    SIM_CTRL = 0x20


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


_TX = struct.Struct("<BB2xIB3xf")
_RQ = struct.Struct("<BB2xIQQ")
_AT = struct.Struct("<BB2xIQf4x")
_CT_HEADER = struct.Struct("<B3xII")
_CFG = struct.Struct("<B256s3xii")
_SIM_CONTROL = struct.Struct("<BB")


def _require_size(data: bytes, expected: int, message_name: str) -> None:
    if len(data) != expected:
        raise LegacyProtocolError(
            f"{message_name} requires {expected} bytes, received {len(data)}"
        )


@dataclass(frozen=True)
class TxDataMessage:
    turbine_id: int
    data_type: TxDataType
    value: float
    payload_length: int = 1

    def pack(self) -> bytes:
        return _TX.pack(
            DataHeader.TX_DATA,
            self.turbine_id,
            int(self.data_type),
            self.payload_length,
            self.value,
        )

    @classmethod
    def unpack(cls, data: bytes) -> "TxDataMessage":
        _require_size(data, _TX.size, cls.__name__)
        header, turbine_id, data_type, payload_length, value = _TX.unpack(data)
        if header != DataHeader.TX_DATA:
            raise LegacyProtocolError("invalid TX_DATA header")
        return cls(turbine_id, TxDataType(data_type), value, payload_length)


@dataclass(frozen=True)
class RqDataMessage:
    turbine_id: int
    data_type: TxDataType
    request_time_ms: int
    expiry_time_ms: int

    def pack(self) -> bytes:
        return _RQ.pack(
            DataHeader.RQ_DATA,
            self.turbine_id,
            int(self.data_type),
            self.request_time_ms,
            self.expiry_time_ms,
        )

    @classmethod
    def unpack(cls, data: bytes) -> "RqDataMessage":
        _require_size(data, _RQ.size, cls.__name__)
        header, turbine_id, data_type, request_time, expiry_time = _RQ.unpack(data)
        if header != DataHeader.RQ_DATA:
            raise LegacyProtocolError("invalid RQ_DATA header")
        return cls(turbine_id, TxDataType(data_type), request_time, expiry_time)


@dataclass(frozen=True)
class AtDataMessage:
    turbine_id: int
    data_type: TxDataType
    attack_time_ms: int
    fake_value: float

    def pack(self) -> bytes:
        return _AT.pack(
            DataHeader.AT_DATA,
            self.turbine_id,
            int(self.data_type),
            self.attack_time_ms,
            self.fake_value,
        )

    @classmethod
    def unpack(cls, data: bytes) -> "AtDataMessage":
        _require_size(data, _AT.size, cls.__name__)
        header, turbine_id, data_type, attack_time, fake_value = _AT.unpack(data)
        if header != DataHeader.AT_DATA:
            raise LegacyProtocolError("invalid AT_DATA header")
        return cls(turbine_id, TxDataType(data_type), attack_time, fake_value)


@dataclass(frozen=True)
class CtDataMessage:
    signal: ControlSignal
    data_type: TxDataType
    enable: tuple[bool, ...]

    def pack(self) -> bytes:
        header = _CT_HEADER.pack(
            DataHeader.CT_DATA,
            int(self.signal),
            int(self.data_type),
        )
        return header + bytes(int(value) for value in self.enable)

    @classmethod
    def unpack(cls, data: bytes) -> "CtDataMessage":
        if len(data) < _CT_HEADER.size:
            raise LegacyProtocolError(
                f"{cls.__name__} requires at least {_CT_HEADER.size} bytes"
            )
        header, signal, data_type = _CT_HEADER.unpack_from(data)
        if header != DataHeader.CT_DATA:
            raise LegacyProtocolError("invalid CT_DATA header")
        enable = tuple(bool(value) for value in data[_CT_HEADER.size :])
        return cls(ControlSignal(signal), TxDataType(data_type), enable)


@dataclass(frozen=True)
class CfgDataMessage:
    team_name: str
    scenario_id: int
    turbine_controller: int

    def pack(self) -> bytes:
        encoded_name = self.team_name.encode("utf-8")[:255]
        encoded_name += b"\0" * (256 - len(encoded_name))
        return _CFG.pack(
            DataHeader.CFG_DATA,
            encoded_name,
            self.scenario_id,
            self.turbine_controller,
        )

    @classmethod
    def unpack(cls, data: bytes) -> "CfgDataMessage":
        _require_size(data, _CFG.size, cls.__name__)
        header, team_name, scenario_id, turbine_controller = _CFG.unpack(data)
        if header != DataHeader.CFG_DATA:
            raise LegacyProtocolError("invalid CFG_DATA header")
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
            raise LegacyProtocolError("invalid SIM_CTRL header")
        return cls(bool(sim_start))


LegacyMessage = (
    TxDataMessage
    | RqDataMessage
    | AtDataMessage
    | CtDataMessage
    | CfgDataMessage
    | SimCtrlMessage
)


def parse_message(data: bytes) -> LegacyMessage:
    if not data:
        raise LegacyProtocolError("empty legacy attack-interface message")

    try:
        header = DataHeader(data[0])
    except ValueError as error:
        raise LegacyProtocolError(f"unknown message header 0x{data[0]:02x}") from error

    message_types = {
        DataHeader.TX_DATA: TxDataMessage,
        DataHeader.RQ_DATA: RqDataMessage,
        DataHeader.AT_DATA: AtDataMessage,
        DataHeader.CT_DATA: CtDataMessage,
        DataHeader.CFG_DATA: CfgDataMessage,
        DataHeader.SIM_CTRL: SimCtrlMessage,
    }
    return message_types[header].unpack(data)
