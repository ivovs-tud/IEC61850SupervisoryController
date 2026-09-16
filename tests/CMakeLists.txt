add_executable(sc_legacy_protocol_tests
    unit/LegacyProtocolTests.cpp
)

target_link_libraries(sc_legacy_protocol_tests PRIVATE
    Catch2::Catch2WithMain
    common
)

include(Catch)
catch_discover_tests(
    sc_legacy_protocol_tests
    TEST_PREFIX "sc.legacy_protocol."
    DISCOVERY_MODE PRE_TEST
)

add_executable(sc_attack_interface_tests
    unit/AttackInterfaceTests.cpp
)

target_include_directories(sc_attack_interface_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(sc_attack_interface_tests PRIVATE
    Catch2::Catch2WithMain
    common
)

catch_discover_tests(
    sc_attack_interface_tests
    TEST_PREFIX "sc.attack_interface."
    DISCOVERY_MODE PRE_TEST
)

add_executable(sc_yaw_lut_tests
    unit/YawLutTests.cpp
)

target_include_directories(sc_yaw_lut_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(sc_yaw_lut_tests PRIVATE
    Catch2::Catch2WithMain
    sc_application
)

catch_discover_tests(
    sc_yaw_lut_tests
    TEST_PREFIX "sc.yaw_lut."
    DISCOVERY_MODE PRE_TEST
)

add_executable(sc_control_calculation_tests
    unit/ControlCalculationTests.cpp
)

target_include_directories(sc_control_calculation_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(sc_control_calculation_tests PRIVATE
    Catch2::Catch2WithMain
    sc_application
)

catch_discover_tests(
    sc_control_calculation_tests
    TEST_PREFIX "sc.control_calculation."
    DISCOVERY_MODE PRE_TEST
)

add_executable(sc_runtime_config_tests
    unit/RuntimeConfigTests.cpp
)

target_include_directories(sc_runtime_config_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(sc_runtime_config_tests PRIVATE
    SC_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

target_link_libraries(sc_runtime_config_tests PRIVATE
    Catch2::Catch2WithMain
    common
    sc_application
)

catch_discover_tests(
    sc_runtime_config_tests
    TEST_PREFIX "sc.runtime_config."
    DISCOVERY_MODE PRE_TEST
)

add_executable(sc_periodic_task_tests
    unit/PeriodicTaskTests.cpp
)

target_link_libraries(sc_periodic_task_tests PRIVATE
    Catch2::Catch2WithMain
    common
)

catch_discover_tests(
    sc_periodic_task_tests
    TEST_PREFIX "sc.periodic_task."
    DISCOVERY_MODE PRE_TEST
)

add_test(
    NAME sc.python_legacy_protocol
    COMMAND ${Python3_EXECUTABLE} -m unittest discover
            -s ${CMAKE_CURRENT_SOURCE_DIR}/protocol
            -p test_legacy_attack_protocol.py
)

set_tests_properties(sc.python_legacy_protocol PROPERTIES
    ENVIRONMENT "PYTHONPATH=${PROJECT_SOURCE_DIR}/python;PYTHONDONTWRITEBYTECODE=1"
)

add_test(
    NAME sc.python_attack_client
    COMMAND ${Python3_EXECUTABLE} -m unittest discover
            -s ${CMAKE_CURRENT_SOURCE_DIR}/protocol
            -p test_attack_client.py
)

set_tests_properties(sc.python_attack_client PROPERTIES
    ENVIRONMENT "PYTHONPATH=${PROJECT_SOURCE_DIR}/python;PYTHONDONTWRITEBYTECODE=1"
)

if(SC_BUILD_INTEGRATION_TESTS)
    execute_process(
        COMMAND ${Python3_EXECUTABLE} -c "import zmq"
        RESULT_VARIABLE SC_PYZMQ_IMPORT_RESULT
        OUTPUT_QUIET
        ERROR_QUIET
    )
    if(NOT SC_PYZMQ_IMPORT_RESULT EQUAL 0)
        message(FATAL_ERROR
            "SC_BUILD_INTEGRATION_TESTS requires pyzmq. "
            "Install the Python integration extra with: "
            "python -m pip install -e .[integration]")
    endif()

    add_executable(sc_legacy_attack_loopback_server
        integration/LegacyAttackLoopbackServer.cpp
        ${PROJECT_SOURCE_DIR}/communication/socket/SocketWrapper.cpp
        ${PROJECT_SOURCE_DIR}/communication/socket/SocketWrapperAt.cpp
        ${PROJECT_SOURCE_DIR}/communication/socket/SocketWrapperDH.cpp
        ${PROJECT_SOURCE_DIR}/communication/socket/SocketWrapperOp.cpp
    )

    target_include_directories(sc_legacy_attack_loopback_server PRIVATE
        ${PROJECT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/communication
        ${PROJECT_SOURCE_DIR}/communication/socket
        ${CPPZMQ_INCLUDE_DIR}
        ${ZMQ_INCLUDE_DIRS}
    )

    target_link_libraries(sc_legacy_attack_loopback_server PRIVATE
        common
        Threads::Threads
        ${ZMQ_LIBRARIES}
    )

    if(WIN32)
        target_link_libraries(sc_legacy_attack_loopback_server PRIVATE ws2_32)
        if(TARGET libzmq)
            add_custom_command(TARGET sc_legacy_attack_loopback_server POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        $<TARGET_FILE:libzmq>
                        $<TARGET_FILE_DIR:sc_legacy_attack_loopback_server>
            )
        endif()
    endif()

    add_test(
        NAME sc.integration.legacy_attack_loopback
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_legacy_attack_loopback.py
                $<TARGET_FILE:sc_legacy_attack_loopback_server>
    )
    set_tests_properties(sc.integration.legacy_attack_loopback PROPERTIES
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1"
        TIMEOUT 30
    )
endif()
