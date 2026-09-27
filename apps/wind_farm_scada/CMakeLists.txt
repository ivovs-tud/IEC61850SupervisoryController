add_executable(wind_farm_scada main.cpp)
add_executable(supervisory_controller ALIAS wind_farm_scada)

set_target_properties(wind_farm_scada PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}"
)

target_link_libraries(wind_farm_scada PRIVATE
    sc::controller
    Threads::Threads
    sc::project_warnings
)

if(WIN32)
    add_custom_command(TARGET wind_farm_scada POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:sc_libiec61850>
                $<TARGET_FILE_DIR:wind_farm_scada>
        COMMENT "Copying libiec61850 runtime"
    )

    if(TARGET libzmq)
        add_custom_command(TARGET wind_farm_scada POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:libzmq>
                    $<TARGET_FILE_DIR:wind_farm_scada>
            COMMENT "Copying ZeroMQ runtime"
        )
    endif()

    set_target_properties(wind_farm_scada PROPERTIES
        VS_DEBUGGER_COMMAND_ARGUMENTS "${DEFAULT_YAW_LUT}"
        VS_DEBUGGER_COMMAND_ARGUMENTS_DEBUG "${DEFAULT_YAW_LUT}"
        VS_DEBUGGER_COMMAND_ARGUMENTS_RELEASE "${DEFAULT_YAW_LUT}"
        VS_DEBUGGER_COMMAND_ARGUMENTS_RELWITHDEBINFO "${DEFAULT_YAW_LUT}"
        VS_DEBUGGER_COMMAND_ARGUMENTS_MINSIZEREL "${DEFAULT_YAW_LUT}"
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        VS_DEBUGGER_WORKING_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}"
        VS_DEBUGGER_WORKING_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}"
        VS_DEBUGGER_WORKING_DIRECTORY_RELWITHDEBINFO "${CMAKE_SOURCE_DIR}"
        VS_DEBUGGER_WORKING_DIRECTORY_MINSIZEREL "${CMAKE_SOURCE_DIR}"
    )

    add_custom_target(run
        COMMAND $<TARGET_FILE:wind_farm_scada> "${DEFAULT_YAW_LUT}"
        DEPENDS wind_farm_scada
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        COMMENT "Running wind_farm_scada with the default yaw LUT"
    )
endif()
