add_library(sc_util STATIC
    Angles.cpp
    Statistics.cpp
    Time.cpp
)

target_include_directories(sc_util PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)

target_compile_features(sc_util PUBLIC cxx_std_17)
target_link_libraries(sc_util PRIVATE sc::project_warnings)
