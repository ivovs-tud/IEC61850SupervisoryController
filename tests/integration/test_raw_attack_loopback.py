import math
import os
import socket
import struct
import subprocess
import sys
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))
from supervisory_controller import (
    AtDataMessage,
    AttackInterface,
    AttackInterfaceError,
    AttackStreamDecoder,
    CfgDataMessage,
    ControlSignal,
    CtDataMessage,
    HeartbeatMessage,
    ReleaseMessage,
    RqDataMessage,
    TxDataMessage,
    TxDataType,
)


def reserve_loopback_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


class RawAttackLoopbackTests(unittest.TestCase):
    def test_client_process_kill_revokes_the_session(self):
        server, port = self.start_server_process()
        child = subprocess.Popen(
            [sys.executable, __file__, "--crash-client", str(port)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        output = ""
        errors = ""
        try:
            self.assertEqual(child.stdout.readline().strip(), "SC_ATTACK_CLIENT_READY")
            child.kill()
            child.communicate(timeout=2.0)
            output, errors = server.communicate(timeout=5.0)
        finally:
            if child.poll() is None:
                child.kill()
                child.communicate(timeout=2.0)
            if server.poll() is None:
                server.terminate()
                output, errors = server.communicate(timeout=2.0)

        counters = self.assert_server_success(server, output, errors)
        self.assert_cleanup(counters, {2, 3})

    def test_python_tcp_client_can_reconnect_to_a_new_server(self):
        first_server, first_port = self.start_server_process()
        client = AttackInterface(num_turbines=2, transport="tcp")
        try:
            self.connect_client(client, first_server, first_port)
            client.configure("first-session")
            client.release()
            first_output, first_errors = first_server.communicate(timeout=5.0)
            self.assertEqual(
                self.assert_server_success(first_server, first_output, first_errors)[
                    "disconnects"
                ],
                1,
            )

            second_server, second_port = self.start_server_process()
            try:
                self.reconnect_client(client, second_server, second_port)
                client.configure("second-session")
                client.release()
                second_output, second_errors = second_server.communicate(timeout=5.0)
            finally:
                if second_server.poll() is None:
                    second_server.terminate()
                    second_output, second_errors = second_server.communicate(timeout=2.0)

            counters = self.assert_server_success(
                second_server, second_output, second_errors
            )
            self.assertEqual(counters["configurations"], 1)
            self.assert_cleanup(counters, {1})
        finally:
            client.stop()
            if first_server.poll() is None:
                first_server.terminate()
                first_server.communicate(timeout=2.0)

    def test_python_tcp_client_uses_the_public_attack_interface(self):
        server, port = self.start_server_process()
        client = AttackInterface(num_turbines=2, transport="tcp")
        output = ""
        errors = ""
        try:
            self.connect_client(client, server, port)

            client.configure("python-tcp-loopback")
            client.begin()
            client.tap_communication("Yaw", [1, 0])
            self.wait_for_client_value(client, "Yaw", 7.0)

            client.tap_communication("Yaw Setpoint", [1, 0])
            client.fdi_communication("Yaw Setpoint", [1, 0])
            client.fdi_next["Yaw Setpoint"][0] = 123.5
            self.wait_for_client_value(client, "Yaw Setpoint", 123.5)

            client.release()
            output, errors = server.communicate(timeout=5.0)
        finally:
            client.stop()
            if server.poll() is None:
                server.terminate()
                output, errors = server.communicate(timeout=2.0)

        counters = self.assert_server_success(server, output, errors)
        self.assertEqual(counters["configurations"], 1)
        self.assert_cleanup(counters, {1})
        self.assertGreaterEqual(counters["overwrite_successes"], 1)

    def test_raw_server_preserves_attack_exchange_and_rejects_second_client(self):
        server, client = self.start_server()
        decoder = AttackStreamDecoder(2)
        output = ""
        errors = ""
        try:
            second = socket.create_connection(client.getpeername(), timeout=2.0)
            second.settimeout(2.0)
            try:
                rejected_data = second.recv(1)
            except ConnectionResetError:
                rejected_data = b""
            self.assertEqual(rejected_data, b"")
            second.close()

            configuration = CfgDataMessage("raw-loopback", 0, 0)
            payload = configuration.pack()
            for byte in payload:
                client.sendall(bytes((byte,)))
            acknowledgement = self.wait_for_message(
                client, decoder, lambda message: message == configuration
            )
            self.assertEqual(acknowledgement, configuration)

            client.sendall(
                b"".join(
                    (
                        CtDataMessage(
                            ControlSignal.CTRL_TAP,
                            TxDataType.TX_YAW,
                            (True, False),
                        ).pack(),
                        CtDataMessage(
                            ControlSignal.CTRL_TAP,
                            TxDataType.TX_SPT_YAW,
                            (True, False),
                        ).pack(),
                        CtDataMessage(
                            ControlSignal.CTRL_FDI,
                            TxDataType.TX_SPT_YAW,
                            (True, False),
                        ).pack(),
                    )
                )
            )

            saw_yaw = False
            saw_request_after_observation = False
            saw_replacement = False
            last_yaw_setpoint = None
            next_heartbeat = time.monotonic()
            deadline = time.monotonic() + 5.0
            while time.monotonic() < deadline and not (
                saw_yaw and saw_request_after_observation and saw_replacement
            ):
                now = time.monotonic()
                if now >= next_heartbeat:
                    client.sendall(HeartbeatMessage().pack())
                    next_heartbeat = now + 0.1
                for message in self.receive_available(client, decoder):
                    if isinstance(message, TxDataMessage):
                        if message.data_type == TxDataType.TX_YAW:
                            saw_yaw = math.isclose(message.value, 7.0)
                        elif message.data_type == TxDataType.TX_SPT_YAW:
                            last_yaw_setpoint = message.value
                            if math.isclose(message.value, 123.5):
                                saw_replacement = True
                    elif isinstance(message, RqDataMessage):
                        if message.data_type == TxDataType.TX_SPT_YAW:
                            saw_request_after_observation = (
                                last_yaw_setpoint is not None
                                and math.isclose(last_yaw_setpoint, -1.0)
                            )
                            client.sendall(
                                AtDataMessage(
                                    message.turbine_id,
                                    message.data_type,
                                    int(time.time_ns() // 1_000_000),
                                    123.5,
                                ).pack()
                            )

            self.assertTrue(saw_yaw)
            self.assertTrue(saw_request_after_observation)
            self.assertTrue(saw_replacement)

            client.sendall(ReleaseMessage().pack())
            client.close()
            output, errors = server.communicate(timeout=5.0)
        finally:
            if client.fileno() >= 0:
                client.close()
            if server.poll() is None:
                server.terminate()
                output, errors = server.communicate(timeout=2.0)

        counters = self.assert_server_success(server, output, errors)
        self.assertEqual(counters["configurations"], 1)
        self.assert_cleanup(counters, {1})
        self.assertGreaterEqual(counters["overwrite_successes"], 1)
        self.assertGreaterEqual(counters["overwrite_timeouts"], 1)

    def test_peer_close_revokes_the_session(self):
        server, client = self.start_server()
        decoder = AttackStreamDecoder(2)
        output = ""
        errors = ""
        try:
            configuration = CfgDataMessage("abrupt-close", 0, 0)
            client.sendall(configuration.pack())
            self.wait_for_message(client, decoder, lambda message: message == configuration)
            client.sendall(
                CtDataMessage(
                    ControlSignal.CTRL_TAP,
                    TxDataType.TX_YAW,
                    (True, False),
                ).pack()
            )
            self.wait_for_message(
                client,
                decoder,
                lambda message: isinstance(message, TxDataMessage)
                and message.data_type == TxDataType.TX_YAW,
            )
            # Keep the read side open while the server processes the peer EOF.
            client.shutdown(socket.SHUT_WR)
            output, errors = server.communicate(timeout=5.0)
        finally:
            if client.fileno() >= 0:
                client.close()
            if server.poll() is None:
                server.terminate()
                output, errors = server.communicate(timeout=2.0)

        counters = self.assert_server_success(server, output, errors)
        self.assertEqual(counters["configurations"], 1)
        # Winsock may surface this half-close as WSAECONNRESET while server
        # messages are still in flight; both paths use the same cleanup.
        expected_reasons = {2, 3} if os.name == "nt" else {2}
        self.assert_cleanup(counters, expected_reasons)

    def test_connection_reset_revokes_the_session(self):
        server, client = self.start_server()
        decoder = AttackStreamDecoder(2)
        output = ""
        errors = ""
        try:
            self.configure_socket(client, decoder, "connection-reset")
            self.wait_for_message(
                client,
                decoder,
                lambda message: isinstance(message, TxDataMessage)
                and message.data_type == TxDataType.TX_YAW,
            )
            linger_format = "hh" if os.name == "nt" else "ii"
            client.setsockopt(
                socket.SOL_SOCKET,
                socket.SO_LINGER,
                struct.pack(linger_format, 1, 0),
            )
            client.close()
            output, errors = server.communicate(timeout=5.0)
        finally:
            if client.fileno() >= 0:
                client.close()
            if server.poll() is None:
                server.terminate()
                output, errors = server.communicate(timeout=2.0)

        counters = self.assert_server_success(server, output, errors)
        self.assert_cleanup(counters, {3})

    def test_malformed_and_truncated_messages_revoke_the_session(self):
        for name, payload in (
            ("unknown-header", b"\xff"),
            (
                "truncated-message",
                CtDataMessage(
                    ControlSignal.CTRL_TAP,
                    TxDataType.TX_PW,
                    (True, False),
                ).pack()[:5],
            ),
        ):
            with self.subTest(name=name):
                server, client = self.start_server()
                decoder = AttackStreamDecoder(2)
                output = ""
                errors = ""
                try:
                    self.configure_socket(client, decoder, name)
                    self.wait_for_message(
                        client,
                        decoder,
                        lambda message: isinstance(message, TxDataMessage)
                        and message.data_type == TxDataType.TX_YAW,
                    )
                    client.sendall(payload)
                    if name == "truncated-message":
                        client.close()
                    output, errors = server.communicate(timeout=5.0)
                finally:
                    if client.fileno() >= 0:
                        client.close()
                    if server.poll() is None:
                        server.terminate()
                        output, errors = server.communicate(timeout=2.0)

                counters = self.assert_server_success(server, output, errors)
                self.assert_cleanup(counters, {4})

    def test_heartbeat_timeout_revokes_within_the_lease_bound(self):
        server, client = self.start_server("lease-timeout")
        decoder = AttackStreamDecoder(2)
        output = ""
        errors = ""
        try:
            self.configure_socket(client, decoder, "lease-timeout")
            client.settimeout(2.0)
            try:
                while client.recv(4096):
                    pass
            except ConnectionResetError:
                pass
            output, errors = server.communicate(timeout=5.0)
        finally:
            if client.fileno() >= 0:
                client.close()
            if server.poll() is None:
                server.terminate()
                output, errors = server.communicate(timeout=2.0)

        counters = self.assert_server_success(server, output, errors)
        self.assert_cleanup(counters, {6})
        # The connected audit event is emitted after the lease timer starts.
        self.assertGreaterEqual(counters["lease_elapsed_ms"], 200)
        self.assertLess(counters["lease_elapsed_ms"], 500)

    def test_server_shutdown_is_reported_by_the_python_client(self):
        server, port = self.start_server_process("shutdown")
        client = AttackInterface(num_turbines=2, transport="tcp")
        output = ""
        errors = ""
        try:
            self.connect_client(client, server, port)
            client.configure("server-shutdown")
            client.begin()
            client.tap_communication("Yaw", [1, 0])
            deadline = time.monotonic() + 5.0
            while client.running and time.monotonic() < deadline:
                time.sleep(0.005)
            with self.assertRaises(AttackInterfaceError):
                client.poll_once()
            output, errors = server.communicate(timeout=5.0)
        finally:
            client.stop()
            if server.poll() is None:
                server.terminate()
                output, errors = server.communicate(timeout=2.0)

        counters = self.assert_server_success(server, output, errors)
        self.assert_cleanup(counters, {7})

    def test_transmit_overflow_closes_and_revokes_the_session(self):
        server, client = self.start_server("overflow")
        decoder = AttackStreamDecoder(2)
        output = ""
        errors = ""
        try:
            self.configure_socket(client, decoder, "overflow")
            client.settimeout(5.0)
            try:
                while client.recv(4096):
                    pass
            except ConnectionResetError:
                pass
            output, errors = server.communicate(timeout=5.0)
        finally:
            if client.fileno() >= 0:
                client.close()
            if server.poll() is None:
                server.terminate()
                output, errors = server.communicate(timeout=2.0)

        counters = self.assert_server_success(server, output, errors)
        self.assertEqual(counters["configurations"], 1)
        self.assert_cleanup(counters, {5})

    def test_slow_reader_cannot_grow_the_server_queue_without_bound(self):
        server, client = self.start_server("slow-reader")
        decoder = AttackStreamDecoder(2)
        output = ""
        errors = ""
        try:
            self.configure_socket(client, decoder, "slow-reader")
            output, errors = server.communicate(timeout=5.0)
        finally:
            if client.fileno() >= 0:
                client.close()
            if server.poll() is None:
                server.terminate()
                output, errors = server.communicate(timeout=2.0)

        counters = self.assert_server_success(server, output, errors)
        self.assert_cleanup(counters, {5})

    def start_server(self, mode=None):
        server, port = self.start_server_process(mode)
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            try:
                client = socket.create_connection(("127.0.0.1", port), timeout=0.2)
                client.settimeout(0.05)
                return server, client
            except OSError:
                if server.poll() is not None:
                    output, errors = server.communicate()
                    self.fail(f"raw attack server exited early: {output}\n{errors}")
                time.sleep(0.01)
        server.terminate()
        server.communicate(timeout=2.0)
        self.fail("timed out connecting to raw attack server")

    def configure_socket(self, client, decoder, label):
        configuration = CfgDataMessage(label, 0, 0)
        client.sendall(configuration.pack())
        self.wait_for_message(client, decoder, lambda message: message == configuration)
        client.sendall(
            b"".join(
                (
                    CtDataMessage(
                        ControlSignal.CTRL_TAP,
                        TxDataType.TX_YAW,
                        (True, False),
                    ).pack(),
                    CtDataMessage(
                        ControlSignal.CTRL_FDI,
                        TxDataType.TX_SPT_YAW,
                        (True, False),
                    ).pack(),
                    CtDataMessage(
                        ControlSignal.CTRL_TAP,
                        TxDataType.TX_SPT_YAW,
                        (True, False),
                    ).pack(),
                )
            )
        )

    def connect_client(self, client, server, port):
        deadline = time.monotonic() + 5.0
        while True:
            try:
                client.connect("127.0.0.1", port)
                return
            except AttackInterfaceError:
                if server.poll() is not None:
                    output, errors = server.communicate()
                    self.fail(f"raw attack server exited early: {output}\n{errors}")
                if time.monotonic() >= deadline:
                    self.fail("timed out connecting the Python TCP attack client")
                time.sleep(0.01)

    def reconnect_client(self, client, server, port):
        deadline = time.monotonic() + 5.0
        while True:
            try:
                client.reconnect("127.0.0.1", port)
                return
            except AttackInterfaceError:
                if server.poll() is not None:
                    output, errors = server.communicate()
                    self.fail(f"raw attack server exited early: {output}\n{errors}")
                if time.monotonic() >= deadline:
                    self.fail("timed out reconnecting the Python TCP attack client")
                time.sleep(0.01)

    def start_server_process(self, mode=None):
        port = reserve_loopback_port()
        command = [sys.argv[1], str(port)]
        if mode is not None:
            command.append(mode)
        server = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        return server, port

    def wait_for_client_value(self, client, signal_name, expected):
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            client.poll_once()
            if math.isclose(client.last_received[signal_name][0], expected):
                return
            time.sleep(0.005)
        self.fail(f"timed out waiting for {signal_name}={expected}")

    def wait_for_message(self, client, decoder, predicate):
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            for message in self.receive_available(client, decoder):
                if predicate(message):
                    return message
        self.fail("timed out waiting for raw attack message")

    def receive_available(self, client, decoder):
        try:
            payload = client.recv(4096)
        except socket.timeout:
            return []
        if not payload:
            self.fail("raw attack server closed the connection unexpectedly")
        return decoder.feed(payload)

    def assert_server_success(self, server, output, errors):
        self.assertEqual(server.returncode, 0, errors)
        result_line = next(
            line for line in output.splitlines()
            if line.startswith("SC_RAW_LOOPBACK_RESULT")
        )
        return {
            key: int(value)
            for key, value in (
                entry.split("=", 1) for entry in result_line.split()[1:]
            )
        }

    def assert_cleanup(self, counters, expected_reasons):
        self.assertEqual(counters["disconnects"], 1)
        self.assertEqual(counters["restored"], 1)
        self.assertIn(counters["reason"], expected_reasons)


def run_crash_client(port):
    client = AttackInterface(num_turbines=2, transport="tcp")
    deadline = time.monotonic() + 5.0
    while True:
        try:
            client.connect("127.0.0.1", port)
            break
        except AttackInterfaceError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.01)
    client.configure("killed-python-client")
    client.begin()
    client.tap_communication("Yaw", [1, 0])
    client.fdi_communication("Yaw Setpoint", [1, 0])
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        client.poll_once()
        if math.isclose(client.last_received["Yaw"][0], 7.0):
            print("SC_ATTACK_CLIENT_READY", flush=True)
            time.sleep(60.0)
            return
        time.sleep(0.005)
    raise RuntimeError("killed-client helper did not observe active tapping")


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--crash-client":
        run_crash_client(int(sys.argv[2]))
    else:
        unittest.main(argv=[sys.argv[0]])
