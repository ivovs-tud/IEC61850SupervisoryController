add_library(sc_runtime STATIC
    DataHistorian.cpp
    PeriodicTask.cpp
    RuntimeConfig.cpp
)
add_library(sc::runtime ALIAS sc_runtime)

target_include_directories(sc_runtime PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(sc_runtime PUBLIC cxx_std_17)

target_link_libraries(sc_runtime
    PUBLIC Boost::headers
    PRIVATE sc::project_warnings
)
