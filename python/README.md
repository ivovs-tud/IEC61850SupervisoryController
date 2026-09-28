# Wind Farm SCADA Attack Client

This directory contains the small, optional Python client used to connect an
attack script to the Wind Farm SCADA application. The controller itself is the
C++ `wind_farm_scada` executable and is built with CMake.

From the repository root, install the client and its ZeroMQ dependency with:

```sh
python -m pip install -e ./python
```

The import package remains `scadaAttackInterface`:

```python
from scadaAttackInterface import AttackInterface

client = AttackInterface(num_turbines=9)
client.connect("127.0.0.1", 9002)
client.configure("PythonAttackClient")
client.tap_communication("Yaw", [1, 0, 0, 0, 0, 0, 0, 0, 0])
```

The complete example is
[`examples/attack_example.py`](examples/attack_example.py). ZeroMQ is the
default transport; pass `transport="tcp"` to use raw TCP. The selected
transport must match `communication.attack_interface.transport` in the SCADA
runtime configuration.
