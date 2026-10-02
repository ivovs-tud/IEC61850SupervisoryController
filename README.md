Implementation of a wind farm supervisory controller in C++, based on the IEC61850-7 communication protocol, with IEC61400-25 data information model. Intended to interface with https://github.com/ivovs-tud/IEC61850ServerPLC due to custom entries in the IEC61400-25 data information model.

The controller exposes operator, HMI, attack/test, and historian interfaces
alongside its IEC 61850 communication. The external interfaces are documented
under [docs/protocols](docs/protocols).

This repository has been developed as part of the EU Horizon program [SUDOCO](https://sudoco.eu/).

## Building from source

The hardware-free tests do not require libiec61850:

```sh
cmake --preset hardware-free-tests
cmake --build --preset hardware-free-tests
ctest --preset hardware-free-tests
```

Contributor formatting and naming conventions are documented in
[docs/development/cpp-style.md](docs/development/cpp-style.md). The formatter
is pinned to clang-format 18.1.8 and can be checked with
`cmake -DSC_FORMAT_MODE=check -P cmake/RunClangFormat.cmake`.

A controller build first looks for a CMake package that provides the target
`libiec61850::iec61850`. When using an existing libiec61850 source build, pass
its location explicitly instead of relying on a machine-specific path:

```sh
cmake -S . -B build/controller \
  -DSC_BUILD_CONTROLLER=ON \
  -DSC_LIBIEC61850_ROOT=/path/to/libiec61850
cmake --build build/controller
```

## License

Copyright (C) 2026 Ivo van Straalen.

This project is licensed under the GNU General Public License, version 3 or
later. See [LICENSE](LICENSE).
