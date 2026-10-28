list(APPEND LIBNUMSTORE_SRCS ${CMAKE_CURRENT_SOURCE_DIR}/ns_aries_tests.c)

add_subdirectory(numstore)
add_subdirectory(regressions)
add_subdirectory(simulation)

set(LIBNUMSTORE_SRCS ${LIBNUMSTORE_SRCS} PARENT_SCOPE)
