import math
import socket
import subprocess
import sys
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))
from supervisory_controller import (
    AtDataMessage,
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
        self.assertEqual(counters["disconnects"], 1)
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
        self.assertEqual(counters["disconnects"], 1)

    def test_transmit_overflow_closes_and_revokes_the_session(self):
        server, client = self.start_server("overflow")
        output = ""
        errors = ""
        try:
            client.sendall(CfgDataMessage("overflow", 0, 0).pack())
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
        self.assertEqual(counters["disconnects"], 1)

    def start_server(self, mode=None):
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


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
