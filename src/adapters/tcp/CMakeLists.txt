add_library(sc_tcp STATIC
    TcpServer.cpp
    AttackChannelTCP.cpp
)

add_library(sc::tcp ALIAS sc_tcp)

target_include_directories(sc_tcp PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

target_compile_features(sc_tcp PUBLIC cxx_std_17)

target_link_libraries(sc_tcp
    PUBLIC sc::runtime
    PRIVATE sc::project_warnings
)

if(WIN32)
    target_link_libraries(sc_tcp PUBLIC ws2_32)
endif()
