import socket
import subprocess
import sys
import time
import unittest
from pathlib import Path

import zmq

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))
from scadaAttackInterface import (
    AttackInterface,
    CfgDataMessage,
    ControlSignal,
    CtDataMessage,
    HeartbeatMessage,
    ReleaseMessage,
    TxDataType,
    parse_message,
)


def reserve_loopback_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


class AttackLoopbackTests(unittest.TestCase):
    def test_cpp_server_and_python_client_exchange_messages(self):
        port = reserve_loopback_port()
        server = subprocess.Popen(
            [sys.argv[1], str(port)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        client = AttackInterface(num_turbines=2)
        output = ""
        errors = ""
        try:
            client.connect("127.0.0.1", port)
            client.configure("loopback-test")
            client.begin()

            client.tap_communication("Yaw", [1, 0])
            self._poll_until(
                client,
                lambda: client.last_received["Yaw"][0] == 7.0,
                "tapped yaw observation",
            )

            client.tap_communication("Yaw Setpoint", [1, 0])
            client.fdi_communication("Yaw Setpoint", [1, 0])

            # Leave FDI without a value long enough to exercise the timeout path.
            time.sleep(0.25)
            client.fdi_next["Yaw Setpoint"][0] = 123.5
            time.sleep(0.5)

            client.release()
            output, errors = server.communicate(timeout=5.0)
        finally:
            client.stop()
            if server.poll() is None:
                server.terminate()
                try:
                    output, errors = server.communicate(timeout=2.0)
                except subprocess.TimeoutExpired:
                    server.kill()
                    output, errors = server.communicate(timeout=2.0)

        self.assertEqual(server.returncode, 0, errors)
        result_line = next(
            line for line in output.splitlines() if line.startswith("SC_LOOPBACK_RESULT")
        )
        counters = {
            key: int(value)
            for key, value in (
                entry.split("=", 1) for entry in result_line.split()[1:]
            )
        }
        self.assertGreaterEqual(counters["configurations"], 1)
        self.assertEqual(counters["disconnects"], 1)
        self.assertEqual(counters["restored"], 1)
        self.assertGreaterEqual(counters["overwrite_successes"], 1)
        self.assertGreaterEqual(counters["overwrite_timeouts"], 1)

    def test_zeromq_queue_overflow_revokes_the_session(self):
        port = reserve_loopback_port()
        server = subprocess.Popen(
            [sys.argv[1], str(port), "slow-reader"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        client = AttackInterface(num_turbines=2)
        output = ""
        errors = ""
        try:
            client.connect("127.0.0.1", port)
            client.configure("zeromq-overflow")
            client.begin()
            client.tap_communication("Yaw", [1, 0])
            output, errors = server.communicate(timeout=5.0)
        finally:
            client.stop()
            if server.poll() is None:
                server.terminate()
                output, errors = server.communicate(timeout=2.0)

        self.assertEqual(server.returncode, 0, errors)
        result_line = next(
            line for line in output.splitlines() if line.startswith("SC_LOOPBACK_RESULT")
        )
        counters = {
            key: int(value)
            for key, value in (
                entry.split("=", 1) for entry in result_line.split()[1:]
            )
        }
        self.assertEqual(counters["configurations"], 1)
        self.assertEqual(counters["disconnects"], 1)
        self.assertEqual(counters["restored"], 1)

    def test_zeromq_drains_control_and_heartbeat_burst_in_one_cycle(self):
        port = reserve_loopback_port()
        server = subprocess.Popen(
            [sys.argv[1], str(port), "burst"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        context = zmq.Context()
        connection = context.socket(zmq.PAIR)
        connection.setsockopt(zmq.LINGER, 0)
        connection.setsockopt(zmq.RCVTIMEO, 2000)
        connection.setsockopt(zmq.SNDTIMEO, 2000)
        connection.connect(f"tcp://127.0.0.1:{port}")
        output = ""
        errors = ""
        try:
            configuration = CfgDataMessage("zeromq-burst", 0, 0)
            connection.send(configuration.pack())
            self.assertEqual(parse_message(connection.recv()), configuration)

            connection.send(CtDataMessage(ControlSignal.CTRL_TAP, TxDataType.TX_YAW, (True, True)).pack())
            connection.send(HeartbeatMessage().pack())
            fdi_signals = (
                TxDataType.TX_WS,
                TxDataType.TX_WD,
                TxDataType.TX_ST,
                TxDataType.TX_PW,
                TxDataType.TX_YAW,
                TxDataType.TX_RPM,
                TxDataType.TX_PTCH,
                TxDataType.TX_SPT_YAW,
                TxDataType.TX_SPT_PWR,
                TxDataType.TX_GENTORQ,
                TxDataType.TX_OP_CMD,
            )
            for signal in fdi_signals:
                connection.send(CtDataMessage(ControlSignal.CTRL_FDI, signal, (True, True)).pack())
            connection.send(HeartbeatMessage().pack())
            connection.send(ReleaseMessage().pack())
            output, errors = server.communicate(timeout=5.0)
        finally:
            connection.close()
            context.term()
            if server.poll() is None:
                server.terminate()
                output, errors = server.communicate(timeout=2.0)

        self.assertEqual(server.returncode, 0, errors)
        result_line = next(
            line for line in output.splitlines() if line.startswith("SC_LOOPBACK_RESULT")
        )
        counters = {
            key: int(value)
            for key, value in (
                entry.split("=", 1) for entry in result_line.split()[1:]
            )
        }
        self.assertEqual(counters["configurations"], 1)
        self.assertEqual(counters["disconnects"], 1)
        self.assertEqual(counters["control_changes"], 2 * (1 + len(fdi_signals)))
        self.assertEqual(counters["client_release"], 1)
        self.assertEqual(counters["restored"], 1)

    def _poll_until(self, client, predicate, description):
        deadline = time.monotonic() + 5.0
        while time.monotonic() < deadline:
            client.poll_once()
            if predicate():
                return
            time.sleep(0.005)
        self.fail(f"timed out waiting for {description}")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
