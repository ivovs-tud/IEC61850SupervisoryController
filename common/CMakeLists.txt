# common is a header-only module; INTERFACE avoids creating an empty archive.
add_library(common INTERFACE)

target_include_directories(common INTERFACE
    ${PROJECT_SOURCE_DIR}   # consumers include as "common/GlobalDataStructure.hpp"
)

# Expose Boost headers to all consumers of common (tasks, communication, main).
target_link_libraries(common INTERFACE Boost::headers)
