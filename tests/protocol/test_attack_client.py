import math
import threading
import time
import unittest
from collections import deque

from scadaAttackInterface import (
    AtDataMessage,
    AttackClient,
    AttackInterface,
    AttackInterfaceError,
    AttackTransport,
    AttackTransportError,
    CfgDataMessage,
    ControlSignal,
    CtDataMessage,
    HeartbeatMessage,
    RqDataMessage,
    ReleaseMessage,
    TxDataMessage,
    TxDataType,
    TcpAttackTransport,
    parse_message,
)


class FakeSocket:
    def __init__(self, auto_ack=True):
        self.incoming = deque()
        self.sent = []
        self.endpoint = None
        self.close_count = 0
        self.auto_ack = auto_ack

    def connect(self, endpoint):
        self.endpoint = endpoint

    def send(self, payload):
        encoded = bytes(payload)
        self.sent.append(encoded)
        if self.auto_ack and encoded[0] == 0x10:
            self.incoming.append(encoded)

    def recv(self, flags=0):
        del flags
        if not self.incoming:
            raise BlockingIOError()
        return self.incoming.popleft()

    def close(self, linger=0):
        del linger
        self.close_count += 1


class StalledTransport(AttackTransport):
    def __init__(self):
        self.incoming = deque()
        self.sent = []
        self.receive_started = threading.Event()
        self.release_receive = threading.Event()
        self.receive_error = None
        self.close_count = 0
        self.closed = False

    def connect(self, server_ip, port):
        del server_ip, port

    def send(self, payload):
        if self.closed:
            raise AttackTransportError("transport is closed")
        encoded = bytes(payload)
        self.sent.append(encoded)
        if encoded[0] == 0x10:
            self.incoming.append(parse_message(encoded))

    def receive(self, timeout=0.0):
        if self.incoming:
            return [self.incoming.popleft()]
        self.receive_started.set()
        self.release_receive.wait(max(timeout, 1.0))
        if self.receive_error is not None:
            raise self.receive_error
        return []

    def flush(self, timeout):
        del timeout
        return not self.closed

    def close(self):
        if self.closed:
            return
        self.closed = True
        self.close_count += 1


class AttackClientTests(unittest.TestCase):
    def make_client(self, num_turbines=2, wall_time_ms=None, monotonic=None):
        socket = FakeSocket()
        client = AttackClient(
            num_turbines,
            socket=socket,
            wall_time_ms=wall_time_ms or (lambda: 1_000),
            monotonic=monotonic,
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
        client.configure("integration-team")
        client.tap_communication(["Yaw", "Power"], [1, 0])
        client.fdi_communication("Yaw Setpoint", [0, 1])

        self.assertEqual(socket.endpoint, "tcp://127.0.0.1:9002")
        self.assertEqual(
            parse_message(socket.sent[0]),
            CfgDataMessage("integration-team", 0, 0),
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
        client.configure("test attack")

        with self.assertRaises(AttackInterfaceError):
            client.tap_communication("Yaw", [1])
        with self.assertRaises(AttackInterfaceError):
            client.tap_communication("not-a-signal", [1, 0])

    def test_signal_names_ignore_capitalization_and_separators(self):
        client, socket = self.make_client()
        client.configure("test attack")
        socket.sent.clear()
        aliases = [
            ("Wind Speed", TxDataType.TX_WS),
            ("wind speed", TxDataType.TX_WS),
            ("windspeed", TxDataType.TX_WS),
            ("  WIND---SPEED  ", TxDataType.TX_WS),
            ("wind_speed", TxDataType.TX_WS),
            ("winddirection", TxDataType.TX_WD),
            ("ROTOR-SPEED", TxDataType.TX_RPM),
            (" yaw ", TxDataType.TX_YAW),
            ("blade___pitch", TxDataType.TX_PTCH),
            ("POWER", TxDataType.TX_PW),
            ("yawsetpoint", TxDataType.TX_SPT_YAW),
            ("power  setpoint", TxDataType.TX_SPT_PWR),
            ("generator-torque", TxDataType.TX_GENTORQ),
        ]

        for alias, expected_type in aliases:
            client.tap_communication(alias, [1, 0])
            self.assertEqual(
                parse_message(socket.sent[-1]),
                CtDataMessage(ControlSignal.CTRL_TAP, expected_type, (True, False)),
            )

        self.assertEqual(set(client._tap_cfg), set(client.SIGNAL_TYPES))
        self.assertTrue(all(client._tap_cfg[name] == [True, False] for name in client.SIGNAL_TYPES))

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
        client.configure("lifecycle test")
        socket.sent.clear()
        client.begin(attack_function)

        self.assertTrue(client.running)
        self.assertTrue(invoked.wait(0.5))
        self.assertEqual(socket.sent, [])

        client.stop()
        client.stop()
        self.assertFalse(client.running)
        self.assertTrue(client.closed)
        self.assertEqual(socket.close_count, 1)
        self.assertEqual(parse_message(socket.sent[-1]), ReleaseMessage())

    def test_slow_attack_callback_does_not_delay_transport_heartbeat(self):
        client, socket = self.make_client()
        callback_started = threading.Event()
        callback_release = threading.Event()

        def slow_attack(data_received, attacks, elapsed_ms):
            del data_received, attacks, elapsed_ms
            callback_started.set()
            callback_release.wait(1.0)

        client.configure("slow callback")
        socket.sent.clear()
        client.begin(slow_attack)
        try:
            self.assertTrue(callback_started.wait(0.5))
            deadline = time.monotonic() + 0.75
            while time.monotonic() < deadline:
                if any(
                    isinstance(parse_message(payload), HeartbeatMessage)
                    for payload in socket.sent
                ):
                    break
                time.sleep(0.005)
            else:
                self.fail("transport heartbeat was delayed by the attack callback")
        finally:
            callback_release.set()
            client.stop()

    def test_poll_sends_heartbeat_at_configured_interval(self):
        now = [0.0]
        client, socket = self.make_client(monotonic=lambda: now[0])
        client.configure("heartbeat test")
        socket.sent.clear()

        client.poll_once()
        self.assertEqual(socket.sent, [])
        now[0] = 0.2
        client.poll_once()
        self.assertEqual(parse_message(socket.sent[-1]), HeartbeatMessage())
        now[0] = 0.3
        client.poll_once()
        self.assertEqual(len(socket.sent), 1)

    def test_heartbeat_publishes_enabled_fdi_values(self):
        now = [0.0]
        client, socket = self.make_client(monotonic=lambda: now[0])
        client.configure("proactive FDI test")
        client.fdi_communication("Yaw Setpoint", [1, 0])
        client.fdi_next["Yaw Setpoint"][0] = 37.5
        socket.sent.clear()

        now[0] = 0.2
        client.poll_once()

        self.assertEqual(parse_message(socket.sent[0]), HeartbeatMessage())
        self.assertEqual(
            parse_message(socket.sent[1]),
            AtDataMessage(1, TxDataType.TX_SPT_YAW, 1_000, 37.5),
        )

    def test_configuration_requires_a_printable_label(self):
        client, _ = self.make_client()

        for label in ("", "   ", "bad\nlabel", "x" * 256):
            with self.subTest(label=label):
                with self.assertRaises(AttackInterfaceError):
                    client.configure(label)

    def test_configuration_requires_server_acknowledgement(self):
        socket = FakeSocket(auto_ack=False)
        client = AttackClient(2, socket=socket)
        client.CONFIGURATION_TIMEOUT_SECONDS = 0.01

        with self.assertRaisesRegex(AttackInterfaceError, "active client"):
            client.configure("unacknowledged")

    def test_tcp_transport_can_be_selected_without_changing_the_client_api(self):
        client = AttackClient(2, transport="tcp")

        self.assertEqual(client.transport, "tcp")
        self.assertIsInstance(client._transport, TcpAttackTransport)
        client.stop()

    def test_transport_failure_clears_session_state_and_cleanup_is_idempotent(self):
        transport = StalledTransport()
        client = AttackClient(2, transport=transport)
        client.configure("transport failure")
        client.tap_communication("Yaw", [1, 0])
        client.fdi_communication("Yaw Setpoint", [1, 0])
        client.fdi_next["Yaw Setpoint"][0] = 45.0
        transport.receive_error = AttackTransportError("peer closed connection")
        transport.release_receive.set()

        client.begin()
        deadline = time.monotonic() + 1.0
        while client.running and time.monotonic() < deadline:
            time.sleep(0.005)

        with self.assertRaisesRegex(AttackInterfaceError, "peer closed"):
            client.poll_once()
        self.assertTrue(all(not enabled for enabled in client._tap_cfg["Yaw"]))
        self.assertTrue(
            all(not enabled for enabled in client._fdi_cfg["Yaw Setpoint"])
        )
        self.assertTrue(math.isnan(client.fdi_next["Yaw Setpoint"][0]))

        client.stop()
        client.stop()
        self.assertEqual(transport.close_count, 1)

    def test_network_stall_cannot_grow_the_outbound_queue_without_bound(self):
        transport = StalledTransport()
        client = AttackClient(2, transport=transport)
        client.OUTBOUND_BUFFER_BYTES = 14
        client.configure("network stall")
        client.begin()
        self.assertTrue(transport.receive_started.wait(0.5))

        client.tap_communication("Yaw", [1, 0])
        with self.assertRaisesRegex(AttackInterfaceError, "outbound buffer overflow"):
            client.fdi_communication("Yaw", [1, 0])

        self.assertLessEqual(client._outbound_bytes, client.OUTBOUND_BUFFER_BYTES)
        self.assertTrue(all(not enabled for enabled in client._tap_cfg["Yaw"]))
        self.assertTrue(all(not enabled for enabled in client._fdi_cfg["Yaw"]))

        transport.release_receive.set()
        with self.assertRaisesRegex(AttackInterfaceError, "outbound buffer overflow"):
            client.poll_once()
        client.stop()

    def test_malformed_message_is_ignored(self):
        client, socket = self.make_client()
        socket.incoming.extend([b"\xff", b"\x01"])

        self.assertIsNone(client.poll_once())
        self.assertIsNone(client.poll_once())

    def test_participant_facing_class_name_remains_available(self):
        client = AttackInterface(1, socket=FakeSocket())
        self.assertIsInstance(client, AttackClient)
        self.assertIs(client._AttackInterfaceExcept, AttackInterfaceError)

if __name__ == "__main__":
    unittest.main()
