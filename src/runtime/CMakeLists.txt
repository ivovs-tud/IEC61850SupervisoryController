add_library(sc_runtime STATIC
    PeriodicTask.cpp
    RuntimeConfig.cpp
)

add_library(sc::runtime ALIAS sc_runtime)
add_library(common ALIAS sc_runtime)

target_include_directories(sc_runtime PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)

target_compile_features(sc_runtime PUBLIC cxx_std_17)

target_link_libraries(sc_runtime
    PUBLIC Boost::headers
    PRIVATE sc::project_warnings
)
