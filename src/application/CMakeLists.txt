add_library(sc_application STATIC
    ControlCalculation.cpp
    Monitoring.cpp
    SignalProcessing.cpp
    YawLut.cpp
)
add_library(sc::application ALIAS sc_application)

target_include_directories(sc_application
    PUBLIC $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
           $<INSTALL_INTERFACE:include>
)

target_compile_features(sc_application PUBLIC cxx_std_17)

target_link_libraries(sc_application
    PUBLIC sc::util
    PRIVATE sc::project_warnings
)
