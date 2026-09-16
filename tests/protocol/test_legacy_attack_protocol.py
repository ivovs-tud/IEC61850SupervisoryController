import unittest

from supervisory_controller.legacy_attack_protocol import (
    AtDataMessage,
    CfgDataMessage,
    ControlSignal,
    CtDataMessage,
    LegacyProtocolError,
    RqDataMessage,
    SimCtrlMessage,
    TxDataMessage,
    TxDataType,
    parse_message,
)


class LegacyAttackProtocolTests(unittest.TestCase):
    def assert_round_trip(self, message, expected_hex):
        payload = message.pack()
        self.assertEqual(payload.hex(), expected_hex)
        self.assertEqual(parse_message(payload), message)

    def test_tx_data_fixture(self):
        self.assert_round_trip(
            TxDataMessage(2, TxDataType.TX_YAW, 12.5),
            "01020000050000000100000000004841",
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

    def test_rejects_invalid_messages(self):
        with self.assertRaises(LegacyProtocolError):
            parse_message(b"")
        with self.assertRaises(LegacyProtocolError):
            parse_message(b"\xff")
        with self.assertRaises(LegacyProtocolError):
            parse_message(b"\x01")


if __name__ == "__main__":
    unittest.main()
