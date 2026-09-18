# common includes both header and implementation files
add_library(common 
    PeriodicTask.cpp
    RuntimeConfig.cpp
)

target_include_directories(common PUBLIC
    ${PROJECT_SOURCE_DIR}   # consumers include as "common/SharedData.hpp"
    ${PROJECT_SOURCE_DIR}/include
)

# Expose Boost headers to all consumers of common (tasks, communication, main).
target_link_libraries(common PUBLIC Boost::headers)
