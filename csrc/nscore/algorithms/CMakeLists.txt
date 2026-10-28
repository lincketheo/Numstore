list(APPEND LIBNUMSTORE_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/ns_node_updates.c)

add_subdirectory(numstore)
add_subdirectory(rope)
add_subdirectory(smartfiles)
add_subdirectory(var)

set(LIBNUMSTORE_SRCS ${LIBNUMSTORE_SRCS} PARENT_SCOPE)
