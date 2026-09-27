include(LibIec61850)
sc_configure_libiec61850()

add_library(sc_iec61850 STATIC
    libiec_wrapper.cpp
    IEC61850Manager.cpp
)

add_library(sc::iec61850 ALIAS sc_iec61850)

target_include_directories(sc_iec61850 PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

target_compile_features(sc_iec61850 PUBLIC cxx_std_17)

target_link_libraries(sc_iec61850
    PUBLIC sc::runtime
    PRIVATE libiec61850::iec61850
            Threads::Threads
            sc::project_warnings
)
