add_library(sc_tasks STATIC
    ControlTask.cpp
    MonitoringTask.cpp
    SignalProcessingTask.cpp
)

target_include_directories(sc_tasks PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)

target_compile_features(sc_tasks PUBLIC cxx_std_17)

target_link_libraries(sc_tasks
    PUBLIC sc_application
           sc_runtime
    PRIVATE sc::project_warnings
)
