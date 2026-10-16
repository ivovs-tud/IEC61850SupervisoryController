# Wind Farm SCADA

Wind Farm SCADA is a C++ supervisory controller based on IEC 61850-7 and the
IEC 61400-25 wind-power information model. It is intended to communicate with
the turbine servers from
[IEC61850ServerPLC](https://github.com/ivovs-tud/IEC61850ServerPLC), which
provide the project-specific IEC 61400-25 data objects.

This repository was developed as part of the EU Horizon
[SUDOCO](https://sudoco.eu/) project.

## Building from source

The build requires:

- CMake 3.21 or newer;
- a C++17 compiler;
- libiec61850;
- Boost, ZeroMQ, cppzmq, and MsgPack.

The dependencies listed in `vcpkg.json` can be installed by CMake through
vcpkg. libiec61850 must currently be built or installed separately. Set
`VCPKG_ROOT` to the vcpkg directory and configure a release build with:

```sh
cmake -S . -B build/release \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
  -DSC_LIBIEC61850_ROOT=/path/to/libiec61850
cmake --build build/release --config Release
```

If the other dependencies are already installed and discoverable by CMake,
omit `CMAKE_TOOLCHAIN_FILE`. `SC_LIBIEC61850_ROOT` may point to either a
libiec61850 installation prefix or its source tree containing an existing
build. It may be omitted when an installed CMake package already provides
`libiec61850::iec61850`.

Use `-DCMAKE_BUILD_TYPE=Debug` and `--config Debug` for a debug build. A normal
build includes the `wind_farm_scada` controller executable. Tests are disabled
unless explicitly enabled.

## Building and running the tests

The complete test suite includes C++, Python, and local network integration
tests. It does not require live IEC 61850 equipment, but it does compile and
link the controller's IEC 61850 integration.

Install the auxiliary Python attack client, set `VCPKG_ROOT`, and use the test
preset:

```sh
python -m pip install ./python
cmake --preset vcpkg-hardware-free-tests -DSC_LIBIEC61850_ROOT=/path/to/libiec61850
cmake --build --preset vcpkg-hardware-free-tests
ctest --preset vcpkg-hardware-free-tests
```

When all dependencies are already installed and discoverable, use the
`hardware-free-tests` preset instead.

## License

Copyright (C) 2026 Ivo van Straalen.

This project is licensed under the GNU General Public License, version 3 or
later. See [LICENSE](LICENSE).
