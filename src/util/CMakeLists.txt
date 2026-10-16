add_library(sc_util STATIC
    Angles.cpp
    Statistics.cpp
    Time.cpp
)
add_library(sc::util ALIAS sc_util)

target_include_directories(sc_util PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)

target_compile_features(sc_util PUBLIC cxx_std_17)
target_link_libraries(sc_util PRIVATE sc::project_warnings)
