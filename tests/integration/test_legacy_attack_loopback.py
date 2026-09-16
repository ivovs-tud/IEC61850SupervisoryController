import math
import socket
import subprocess
import sys
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "python"))
from supervisory_controller import AttackInterface, SimCtrlMessage


def reserve_loopback_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(("127.0.0.1", 0))
        return probe.getsockname()[1]


class LegacyAttackLoopbackTests(unittest.TestCase):
    def test_cpp_server_and_python_client_exchange_legacy_messages(self):
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
            client.configure("loopback-test", scenario_id=7, turbine_controller=2)
            client.begin(wait_for_ready=True, ready_timeout=5.0)

            client.tap_communication("Yaw", [1, 0])
            self._poll_until(
                client,
                lambda: client.last_received["Yaw"][0] == 7.0,
                "tapped yaw observation",
            )

            client._socket.send(b"\xff")
            client.tap_communication("Yaw Setpoint", [1, 0])
            client.fdi_communication("Yaw Setpoint", [1, 0])
            client.fdi_next["Yaw Setpoint"][0] = 123.5

            time.sleep(0.25)
            self._poll_until(
                client,
                lambda: math.isclose(
                    client.last_received["Yaw Setpoint"][0], 123.5
                ),
                "FDI overwrite observation",
            )

            client._socket.send(SimCtrlMessage(False).pack())
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
        self.assertGreaterEqual(counters["overwrite_successes"], 1)
        self.assertGreaterEqual(counters["overwrite_timeouts"], 1)

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
