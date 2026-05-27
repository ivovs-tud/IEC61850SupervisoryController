# common includes both header and implementation files
add_library(common 
    PeriodicTask.cpp
)

target_include_directories(common PUBLIC
    ${PROJECT_SOURCE_DIR}   # consumers include as "common/GlobalDataStructure.hpp"
)

# Expose Boost headers to all consumers of common (tasks, communication, main).
target_link_libraries(common PUBLIC Boost::headers)


