"""Example attack client for the wind-farm SCADA controller."""

import logging

from scadaAttackInterface import AttackInterface


SERVER_IP = "127.0.0.1"
PORT = 9002
TRANSPORT = "zeromq"  # Must match communication.attack_interface.transport.
NUM_TURBINES = 9
ENABLED_TURBINES = [1] * NUM_TURBINES


def attack_function(data_received: dict[str, list[float]], attacks: dict[str, list[float]], time_ms: int) -> None:
    """Set every enabled turbine's yaw setpoint to 90 degrees."""
    logging.debug("Attack time: %d ms; received data: %s", time_ms, data_received)
    for turbine_index in range(NUM_TURBINES):
        attacks["Yaw Setpoint"][turbine_index] = 90.0


def main() -> None:
    logging.basicConfig(level=logging.INFO, format="[%(asctime)s] %(levelname)s: %(message)s")

    attack_interface = AttackInterface(num_turbines=NUM_TURBINES, transport=TRANSPORT)
    try:
        attack_interface.connect(SERVER_IP, PORT)
        attack_interface.configure("PythonAttackClient")

        attack_interface.tap_communication(list(attack_interface.SIGNAL_TYPES), ENABLED_TURBINES)
        attack_interface.fdi_communication("Yaw Setpoint", ENABLED_TURBINES)

        print("Starting attack interface. Press Ctrl+C to stop.")
        attack_interface.start(attack_function)
        
    except KeyboardInterrupt:
        pass

    finally:
        attack_interface.stop()

    print("Attack interface stopped.")


if __name__ == "__main__":
    main()
