add_library(sc_application STATIC
    ControlCalculation.cpp
    Monitoring.cpp
    SignalProcessing.cpp
    YawLut.cpp
)

target_include_directories(sc_application
    PUBLIC ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(sc_application PUBLIC cxx_std_17)

target_link_libraries(sc_application
    PUBLIC sc_util
    PRIVATE sc::project_warnings
)
