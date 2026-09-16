import math
import threading
import unittest
from collections import deque

from supervisory_controller import (
    AtDataMessage,
    AttackClientDependencyError,
    AttackInterface,
    AttackInterfaceError,
    CfgDataMessage,
    ControlSignal,
    CtDataMessage,
    LegacyAttackClient,
    RqDataMessage,
    SimCtrlMessage,
    TxDataMessage,
    TxDataType,
    parse_message,
)
from supervisory_controller import attack_client


class FakeSocket:
    def __init__(self):
        self.incoming = deque()
        self.sent = []
        self.endpoint = None
        self.close_count = 0

    def connect(self, endpoint):
        self.endpoint = endpoint

    def send(self, payload):
        self.sent.append(bytes(payload))

    def recv(self, flags=0):
        del flags
        if not self.incoming:
            raise BlockingIOError()
        return self.incoming.popleft()

    def close(self, linger=0):
        del linger
        self.close_count += 1


class AttackClientTests(unittest.TestCase):
    def make_client(self, num_turbines=2, wall_time_ms=None):
        socket = FakeSocket()
        client = LegacyAttackClient(
            num_turbines,
            socket=socket,
            wall_time_ms=wall_time_ms or (lambda: 1_000),
        )
        return client, socket

    def test_initial_state_has_one_value_per_turbine(self):
        client, _ = self.make_client(3)

        self.assertTrue(all(len(values) == 3 for values in client.last_received.values()))
        self.assertTrue(all(len(values) == 3 for values in client.fdi_next.values()))
        self.assertTrue(all(math.isnan(value) for value in client.fdi_next["Yaw"]))

    def test_connect_configure_and_control_use_shared_codec(self):
        client, socket = self.make_client()
        client.connect("127.0.0.1", 9002)
        client.configure("integration-team", scenario_id=7, turbine_controller=2)
        client.tap_communication(["Yaw", "Power"], [1, 0])
        client.fdi_communication("Yaw Setpoint", [0, 1])

        self.assertEqual(socket.endpoint, "tcp://127.0.0.1:9002")
        self.assertEqual(
            parse_message(socket.sent[0]),
            CfgDataMessage("integration-team", 7, 2),
        )
        self.assertEqual(
            parse_message(socket.sent[1]),
            CtDataMessage(ControlSignal.CTRL_TAP, TxDataType.TX_YAW, (True, False)),
        )
        self.assertEqual(
            parse_message(socket.sent[2]),
            CtDataMessage(ControlSignal.CTRL_TAP, TxDataType.TX_PW, (True, False)),
        )
        self.assertEqual(
            parse_message(socket.sent[3]),
            CtDataMessage(
                ControlSignal.CTRL_FDI,
                TxDataType.TX_SPT_YAW,
                (False, True),
            ),
        )

    def test_rejects_invalid_control_arguments(self):
        client, _ = self.make_client()

        with self.assertRaises(AttackInterfaceError):
            client.tap_communication("Yaw", [1])
        with self.assertRaises(AttackInterfaceError):
            client.tap_communication("not-a-signal", [1, 0])

    def test_poll_updates_tapped_data_and_answers_valid_fdi_request(self):
        client, socket = self.make_client(wall_time_ms=lambda: 1_000)
        client.fdi_next["Yaw Setpoint"][1] = 42.5
        socket.sent.clear()
        socket.incoming.extend(
            [
                TxDataMessage(1, TxDataType.TX_YAW, 15.25).pack(),
                RqDataMessage(2, TxDataType.TX_SPT_YAW, 900, 1_100).pack(),
            ]
        )

        client.poll_once()
        client.poll_once()

        self.assertEqual(client.last_received["Yaw"][0], 15.25)
        self.assertEqual(
            parse_message(socket.sent[0]),
            AtDataMessage(2, TxDataType.TX_SPT_YAW, 1_000, 42.5),
        )

    def test_expired_or_out_of_range_request_is_ignored(self):
        client, socket = self.make_client(wall_time_ms=lambda: 2_000)
        socket.incoming.extend(
            [
                RqDataMessage(1, TxDataType.TX_PW, 900, 1_100).pack(),
                RqDataMessage(3, TxDataType.TX_PW, 1_900, 2_100).pack(),
            ]
        )

        client.poll_once()
        client.poll_once()

        self.assertEqual(socket.sent, [])

    def test_begin_and_stop_have_a_finite_idempotent_lifecycle(self):
        client, socket = self.make_client()
        invoked = threading.Event()

        def attack_function(data_received, attacks, elapsed_ms):
            del data_received, attacks, elapsed_ms
            invoked.set()

        client.ATTACK_INTERVAL_SECONDS = 0.001
        socket.incoming.append(SimCtrlMessage(True).pack())
        client.begin(attack_function, ready_timeout=0.1)

        self.assertTrue(client.running)
        self.assertTrue(invoked.wait(0.5))
        self.assertEqual(parse_message(socket.sent[0]), SimCtrlMessage(True))

        client.stop()
        client.stop()
        self.assertFalse(client.running)
        self.assertTrue(client.closed)
        self.assertEqual(socket.close_count, 1)

    def test_malformed_message_is_ignored(self):
        client, socket = self.make_client()
        socket.incoming.extend([b"\xff", b"\x01"])

        self.assertIsNone(client.poll_once())
        self.assertIsNone(client.poll_once())

    def test_legacy_class_name_remains_available(self):
        client = AttackInterface(1, socket=FakeSocket())
        self.assertIsInstance(client, LegacyAttackClient)
        self.assertIs(client._AttackInterfaceExcept, AttackInterfaceError)

    def test_missing_pyzmq_only_fails_when_a_real_socket_is_requested(self):
        saved_zmq = attack_client._zmq
        attack_client._zmq = None
        try:
            injected = LegacyAttackClient(1, socket=FakeSocket())
            self.assertFalse(injected.closed)
            with self.assertRaises(AttackClientDependencyError):
                LegacyAttackClient(1)
        finally:
            attack_client._zmq = saved_zmq


if __name__ == "__main__":
    unittest.main()
