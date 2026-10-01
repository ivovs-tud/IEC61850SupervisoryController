function(sc_configure_libiec61850)
    if(TARGET libiec61850::iec61850)
        return()
    endif()

    find_package(libiec61850 CONFIG QUIET)
    if(TARGET libiec61850::iec61850)
        return()
    elseif(TARGET iec61850::iec61850)
        add_library(libiec61850::iec61850 ALIAS iec61850::iec61850)
        return()
    elseif(TARGET iec61850)
        add_library(libiec61850::iec61850 ALIAS iec61850)
        return()
    endif()

    set(SC_LIBIEC61850_ROOT "" CACHE PATH
        "libiec61850 installation prefix or source tree containing an existing build")

    set(_sc_libiec_hints)
    if(SC_LIBIEC61850_ROOT)
        list(APPEND _sc_libiec_hints "${SC_LIBIEC61850_ROOT}")
    endif()

    find_path(SC_LIBIEC61850_INCLUDE_DIR
        NAMES iec61850_client.h
        HINTS ${_sc_libiec_hints}
        PATH_SUFFIXES include src/iec61850/inc
    )
    find_library(SC_LIBIEC61850_LIBRARY
        NAMES iec61850 libiec61850
        HINTS ${_sc_libiec_hints}
        PATH_SUFFIXES lib build build/src build/src/Debug build/src/Release
    )

    if(NOT SC_LIBIEC61850_INCLUDE_DIR OR NOT SC_LIBIEC61850_LIBRARY)
        message(FATAL_ERROR
            "libiec61850 was not found. Install a CMake package that provides "
            "libiec61850::iec61850, or configure with "
            "-DSC_LIBIEC61850_ROOT=/path/to/libiec61850.")
    endif()

    set(_sc_libiec_includes "${SC_LIBIEC61850_INCLUDE_DIR}")
    if(SC_LIBIEC61850_ROOT AND EXISTS "${SC_LIBIEC61850_ROOT}/src/iec61850/inc/iec61850_client.h")
        list(APPEND _sc_libiec_includes
            "${SC_LIBIEC61850_ROOT}/src/common/inc"
            "${SC_LIBIEC61850_ROOT}/src/iec61850/inc"
            "${SC_LIBIEC61850_ROOT}/src/mms/inc"
            "${SC_LIBIEC61850_ROOT}/src/goose"
            "${SC_LIBIEC61850_ROOT}/src/r_session"
            "${SC_LIBIEC61850_ROOT}/hal/inc"
            "${SC_LIBIEC61850_ROOT}/src/logging"
        )
    endif()

    if(WIN32)
        find_file(SC_LIBIEC61850_RUNTIME_LIBRARY
            NAMES iec61850.dll libiec61850.dll
            HINTS ${_sc_libiec_hints}
            PATH_SUFFIXES bin build/src/Debug build/src/Release
        )
    endif()

    if(WIN32 AND SC_LIBIEC61850_RUNTIME_LIBRARY)
        add_library(sc_libiec61850 SHARED IMPORTED GLOBAL)
        set_target_properties(sc_libiec61850 PROPERTIES
            IMPORTED_IMPLIB "${SC_LIBIEC61850_LIBRARY}"
            IMPORTED_LOCATION "${SC_LIBIEC61850_RUNTIME_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${_sc_libiec_includes}"
        )
    else()
        add_library(sc_libiec61850 UNKNOWN IMPORTED GLOBAL)
        set_target_properties(sc_libiec61850 PROPERTIES
            IMPORTED_LOCATION "${SC_LIBIEC61850_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${_sc_libiec_includes}"
        )
    endif()

    add_library(libiec61850::iec61850 ALIAS sc_libiec61850)
endfunction()
