import unittest

from hmi_endpoints import resolve_hmi_endpoints


class HmiEndpointTests(unittest.TestCase):
    def test_windows_defaults_to_localhost(self) -> None:
        self.assertEqual(
            resolve_hmi_endpoints([], "win32"),
            ("tcp://localhost:5555", "tcp://localhost:5556"),
        )

    def test_posix_defaults_to_ipc(self) -> None:
        self.assertEqual(
            resolve_hmi_endpoints([], "linux"),
            (
                "ipc:///tmp/supervisory_controller_hmi.sock",
                "ipc:///tmp/supervisory_controller_hmi_cmd.sock",
            ),
        )

    def test_host_argument_builds_standard_tcp_endpoints(self) -> None:
        self.assertEqual(
            resolve_hmi_endpoints(["controller.local"], "linux"),
            ("tcp://controller.local:5555", "tcp://controller.local:5556"),
        )

    def test_complete_publisher_endpoint_is_not_treated_as_a_host(self) -> None:
        self.assertEqual(
            resolve_hmi_endpoints(["tcp://controller.local:9004"], "linux"),
            ("tcp://controller.local:9004", "tcp://controller.local:5556"),
        )

    def test_explicit_publisher_and_command_endpoints_are_preserved(self) -> None:
        self.assertEqual(
            resolve_hmi_endpoints(
                ["tcp://controller.local:7000", "tcp://controller.local:7001"],
                "linux",
            ),
            ("tcp://controller.local:7000", "tcp://controller.local:7001"),
        )

    def test_invalid_argument_shapes_are_rejected(self) -> None:
        with self.assertRaises(ValueError):
            resolve_hmi_endpoints(["one", "two", "three"], "linux")
        with self.assertRaises(ValueError):
            resolve_hmi_endpoints(["localhost", "localhost"], "linux")


if __name__ == "__main__":
    unittest.main()
