# Tests

Tests are intentionally disabled in the default build. A normal configure does
not search for or require Catch2 or Python:

```sh
cmake -S . -B build \
  -DSC_LIBIEC61850_ROOT=/path/to/libiec61850
cmake --build build
```

To build and run the complete test suite, provide Catch2 3 and install the
Python package (including `pyzmq`), then enable the test option:

```sh
python -m pip install -e ./python
cmake -S . -B build-tests \
  -DSC_BUILD_TESTS=ON \
  -DSC_LIBIEC61850_ROOT=/path/to/libiec61850
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure
```

When using this repository's vcpkg manifest, opt into its test-only feature:

```sh
cmake -S . -B build-tests \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_MANIFEST_FEATURES=tests \
  -DSC_BUILD_TESTS=ON \
  -DSC_LIBIEC61850_ROOT=/path/to/libiec61850
```

The C++ characterization tests protect the historian's remaining native ABI
and the attack interface's explicitly encoded wire bytes. The Python
attack-codec tests protect the same byte-level attack-interface fixtures.

The attack-interface unit tests use in-memory channel and clock fakes. They
exercise the production parser and FDI state machine without opening a socket,
waiting in real time, or connecting to IEC 61850 equipment.

The Python attack-client tests inject a fake socket but import the production
client, so `pyzmq` is installed as a normal package dependency.

`SC_BUILD_TESTS=ON` includes the unit, protocol, and network loopback tests. The
loopback servers do not connect to physical IEC 61850 equipment, but the
complete controller is still part of the project build.

The portable CMake preset runs the same complete hardware-free suite and
expects `pyzmq` in the selected Python environment:

```sh
cmake --preset hardware-free-tests \
  -DSC_LIBIEC61850_ROOT=/path/to/libiec61850
cmake --build --preset hardware-free-tests
ctest --preset hardware-free-tests
```

For a vcpkg-managed build, set `VCPKG_ROOT` and use
`vcpkg-hardware-free-tests`. Native Ubuntu, Windows, and macOS GitHub
Actions jobs run that preset. CI builds and caches a pinned libiec61850 release,
then compiles the complete controller and runs every registered test without
connecting to physical IEC 61850 equipment.
