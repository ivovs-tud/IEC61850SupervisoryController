import math
import struct
import unittest

from scadaAttackInterface.attack_protocol import (
    AttackProtocolError,
    AttackStreamDecoder,
    AtDataMessage,
    CfgDataMessage,
    ControlSignal,
    CtDataMessage,
    DataHeader,
    HeartbeatMessage,
    RqDataMessage,
    ReleaseMessage,
    SimCtrlMessage,
    TxDataMessage,
    TxDataType,
    message_size,
    parse_message,
)


class AttackProtocolTests(unittest.TestCase):
    def assert_round_trip(self, message, expected_hex):
        payload = message.pack()
        self.assertEqual(payload.hex(), expected_hex)
        self.assertEqual(parse_message(payload), message)

    def test_tx_data_fixture(self):
        self.assert_round_trip(
            TxDataMessage(2, TxDataType.TX_YAW, 12.5),
            "01020000050000000100000000004841",
        )

    def test_tx_data_preserves_integer_values(self):
        self.assert_round_trip(
            TxDataMessage(2, TxDataType.TX_ST, 3),
            "01020000030000000100000003000000",
        )

    def test_request_fixture(self):
        self.assert_round_trip(
            RqDataMessage(3, TxDataType.TX_PW, 1000, 1500),
            "0203000004000000e803000000000000dc05000000000000",
        )

    def test_attack_fixture(self):
        self.assert_round_trip(
            AtDataMessage(3, TxDataType.TX_PW, 1100, -7.25),
            "04030000040000004c040000000000000000e8c000000000",
        )

    def test_control_fixture(self):
        self.assert_round_trip(
            CtDataMessage(
                ControlSignal.CTRL_FDI,
                TxDataType.TX_YAW,
                (True, False, True, False, False, False, False, False, False),
            ),
            "080000000200000005000000010001000000000000",
        )

    def test_config_round_trip(self):
        message = CfgDataMessage("PythonAttackClient", 7, 2)
        self.assertEqual(len(message.pack()), 268)
        self.assertEqual(parse_message(message.pack()), message)

    def test_simulation_control_fixture(self):
        self.assert_round_trip(SimCtrlMessage(True), "2001")

    def test_session_control_fixtures(self):
        self.assert_round_trip(HeartbeatMessage(), "40")
        self.assert_round_trip(ReleaseMessage(), "80")

    def test_rejects_invalid_messages(self):
        with self.assertRaises(AttackProtocolError):
            parse_message(b"")
        with self.assertRaises(AttackProtocolError):
            parse_message(b"\xff")
        with self.assertRaises(AttackProtocolError):
            parse_message(b"\x01")

        invalid_length = bytearray(TxDataMessage(2, TxDataType.TX_YAW, 12.5).pack())
        invalid_length[8] = 2
        with self.assertRaises(AttackProtocolError):
            parse_message(invalid_length)

    def test_rejects_invalid_value_types_and_infinities(self):
        with self.assertRaises(AttackProtocolError):
            TxDataMessage(1, TxDataType.TX_ST, 3.0).pack()
        with self.assertRaises(AttackProtocolError):
            TxDataMessage(1, TxDataType.TX_WS, float("inf")).pack()
        with self.assertRaises(AttackProtocolError):
            AtDataMessage(1, TxDataType.TX_WS, 100, float("-inf")).pack()

        infinite_observation = bytearray(
            TxDataMessage(1, TxDataType.TX_WS, 1.0).pack()
        )
        infinite_observation[12:16] = struct.pack("<f", float("inf"))
        with self.assertRaises(AttackProtocolError):
            parse_message(infinite_observation)

        infinite_replacement = bytearray(
            AtDataMessage(1, TxDataType.TX_WS, 100, 1.0).pack()
        )
        infinite_replacement[16:20] = struct.pack("<f", float("-inf"))
        with self.assertRaises(AttackProtocolError):
            parse_message(infinite_replacement)

        no_replacement = AtDataMessage(
            1, TxDataType.TX_WS, 100, float("nan")
        )
        decoded = parse_message(no_replacement.pack())
        self.assertIsInstance(decoded, AtDataMessage)
        self.assertTrue(math.isnan(decoded.fake_value))


class AttackStreamDecoderTests(unittest.TestCase):
    def messages(self):
        return [
            TxDataMessage(2, TxDataType.TX_YAW, 12.5),
            RqDataMessage(3, TxDataType.TX_PW, 1000, 1500),
            AtDataMessage(3, TxDataType.TX_PW, 1100, -7.25),
            CtDataMessage(
                ControlSignal.CTRL_FDI,
                TxDataType.TX_YAW,
                (True, False, True, False, False, False, False, False, False),
            ),
            CfgDataMessage("PythonAttackClient", 7, 2),
            SimCtrlMessage(True),
            HeartbeatMessage(),
            ReleaseMessage(),
        ]

    def test_accepts_every_split_boundary(self):
        for expected in self.messages():
            payload = expected.pack()
            for split in range(len(payload) + 1):
                with self.subTest(message=type(expected).__name__, split=split):
                    decoder = AttackStreamDecoder(9)
                    decoded = decoder.feed(payload[:split])
                    decoded.extend(decoder.feed(payload[split:]))
                    self.assertEqual(decoded, [expected])
                    self.assertEqual(decoder.buffered_bytes, 0)
                    decoder.finish()

    def test_decodes_coalesced_messages_in_order(self):
        expected = self.messages()
        decoder = AttackStreamDecoder(9)
        self.assertEqual(decoder.feed(b"".join(message.pack() for message in expected)), expected)

    def test_rejects_unknown_truncated_and_oversized_input(self):
        with self.assertRaises(AttackProtocolError):
            AttackStreamDecoder(9).feed(b"\xff")

        invalid_control = bytearray(
            CtDataMessage(
                ControlSignal.CTRL_TAP,
                TxDataType.TX_WS,
                (True,) * 9,
            ).pack()
        )
        invalid_control[-1] = 2
        with self.assertRaises(AttackProtocolError):
            AttackStreamDecoder(9).feed(invalid_control)

        truncated = AttackStreamDecoder(9)
        truncated.feed(CfgDataMessage("truncated", 0, 0).pack()[:-1])
        with self.assertRaises(AttackProtocolError):
            truncated.finish()

        oversized = AttackStreamDecoder(9, max_buffer_bytes=8)
        with self.assertRaises(AttackProtocolError):
            oversized.feed(CfgDataMessage("oversized", 0, 0).pack()[:9])

    def test_control_size_uses_configured_turbine_count(self):
        message = CtDataMessage(
            ControlSignal.CTRL_TAP,
            TxDataType.TX_WS,
            (True, False, True),
        )
        payload = message.pack()
        decoder = AttackStreamDecoder(3)

        self.assertEqual(message_size(DataHeader.CT_DATA, 3), 15)
        self.assertEqual(decoder.feed(payload[:-1]), [])
        self.assertEqual(decoder.feed(payload[-1:]), [message])


if __name__ == "__main__":
    unittest.main()
