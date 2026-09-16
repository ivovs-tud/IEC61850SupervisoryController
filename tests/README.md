# Tests

Tests are intentionally disabled in the default build. A normal configure does
not search for or require Catch2 or Python:

```sh
cmake -S . -B build
cmake --build build
```

To build and run the tests, provide Catch2 3 and a Python 3 interpreter, then
enable the test option:

```sh
cmake -S . -B build-tests -DSC_BUILD_TESTS=ON
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure
```

When using this repository's vcpkg manifest, opt into its test-only feature:

```sh
cmake -S . -B build-tests \
  -DCMAKE_TOOLCHAIN_FILE=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DVCPKG_MANIFEST_FEATURES=tests \
  -DSC_BUILD_TESTS=ON
```

The C++ characterization tests protect current native ABI layouts while those
interfaces are migrated. The Python protocol tests use only the standard
library and protect the byte-level legacy attack-interface fixtures.

The attack-interface unit tests use in-memory channel and clock fakes. They
exercise the production parser and FDI state machine without opening a socket,
waiting in real time, or connecting to IEC 61850 equipment.

The Python attack-client tests similarly inject a standard-library fake socket.
They do not require `pyzmq`; that dependency remains in the optional `attack`
and `integration` Python extras until real loopback tests are enabled.

The C++/Python ZeroMQ loopback test is separately gated. It builds a test-only
server from the production attack socket sources and does not link
libiec61850:

```sh
python -m pip install -e ".[integration]"
cmake -S . -B build-integration \
  -DSC_BUILD_CONTROLLER=OFF \
  -DSC_BUILD_TESTS=ON \
  -DSC_BUILD_INTEGRATION_TESTS=ON
cmake --build build-integration
ctest --test-dir build-integration --output-on-failure
```

Portable CMake presets provide the same hardware-free configurations. The
integration preset expects `pyzmq` in the selected Python environment:

```sh
cmake --preset hardware-free-tests
cmake --build --preset hardware-free-tests
ctest --preset hardware-free-tests

cmake --preset hardware-free-integration
cmake --build --preset hardware-free-integration
ctest --preset hardware-free-integration
```

For a vcpkg-managed build, set `VCPKG_ROOT` and use
`vcpkg-hardware-free-integration`. Native Ubuntu and Windows GitHub Actions
jobs run that preset. The CI job intentionally does not build the IEC-backed
controller; portable libiec61850 discovery remains a later roadmap step.
