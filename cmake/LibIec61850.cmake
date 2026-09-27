function(sc_configure_libiec61850)
    if(TARGET libiec61850::iec61850)
        return()
    endif()

    if(WIN32)
        set(_sc_libiec_default "H:/libiec61850")
    else()
        set(_sc_libiec_default "/home/ivovs/TestbedProjects/libiec61850")
    endif()
    set(LIBIEC61850_DIR "${_sc_libiec_default}" CACHE PATH "Path to libiec61850 source")

    add_library(sc_libiec61850 SHARED IMPORTED GLOBAL)
    add_library(libiec61850::iec61850 ALIAS sc_libiec61850)

    set(_sc_libiec_includes
        "${LIBIEC61850_DIR}/src/common/inc"
        "${LIBIEC61850_DIR}/src/iec61850/inc"
        "${LIBIEC61850_DIR}/src/mms/inc"
        "${LIBIEC61850_DIR}/src/goose"
        "${LIBIEC61850_DIR}/src/r_session"
        "${LIBIEC61850_DIR}/hal/inc"
        "${LIBIEC61850_DIR}/src/logging"
    )

    if(WIN32)
        set_target_properties(sc_libiec61850 PROPERTIES
            IMPORTED_LOCATION "${LIBIEC61850_DIR}/build/src/Debug/iec61850.dll"
            IMPORTED_IMPLIB "${LIBIEC61850_DIR}/build/src/Debug/iec61850.lib"
            INTERFACE_INCLUDE_DIRECTORIES "${_sc_libiec_includes}"
        )
    elseif(APPLE)
        set_target_properties(sc_libiec61850 PROPERTIES
            IMPORTED_LOCATION "${LIBIEC61850_DIR}/build/src/libiec61850.dylib"
            INTERFACE_INCLUDE_DIRECTORIES "${_sc_libiec_includes}"
        )
    else()
        set_target_properties(sc_libiec61850 PROPERTIES
            IMPORTED_LOCATION "${LIBIEC61850_DIR}/build/src/libiec61850.so"
            INTERFACE_INCLUDE_DIRECTORIES "${_sc_libiec_includes}"
        )
    endif()
endfunction()
