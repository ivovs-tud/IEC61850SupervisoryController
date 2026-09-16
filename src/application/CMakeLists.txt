add_library(sc_application STATIC
    ControlCalculation.cpp
    YawLut.cpp
)

target_include_directories(sc_application
    PUBLIC ${PROJECT_SOURCE_DIR}/include
)

target_compile_features(sc_application PUBLIC cxx_std_17)
