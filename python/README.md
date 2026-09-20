# Supervisory Controller Python Client

Install the package and its ZeroMQ dependency with:

```sh
python -m pip install -e .
```

The participant-facing class remains named `AttackInterface`:

```python
from supervisory_controller import AttackInterface

client = AttackInterface(num_turbines=9)
client.connect("127.0.0.1", 9002)
client.configure("PythonAttackClient")
client.tap_communication("Yaw", [1, 0, 0, 0, 0, 0, 0, 0, 0])
```

`start(attack_function)` provides the blocking attack workflow. Applications
that own an event loop can instead call `begin()`, `poll_once()`, and `stop()`,
or use `run_forever()` after `begin()`. Attack callbacks should return promptly
so `stop()` can join the callback thread deterministically.
