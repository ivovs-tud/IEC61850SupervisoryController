add_library(sc_application STATIC
    AttackSessionManager.cpp
    ControlCalculation.cpp
    Monitoring.cpp
    SignalProcessing.cpp
    YawLut.cpp
)

target_include_directories(sc_application
    PUBLIC ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(sc_application PUBLIC cxx_std_17)

add_library(sc::application ALIAS sc_application)

target_link_libraries(sc_application PRIVATE sc::project_warnings)
