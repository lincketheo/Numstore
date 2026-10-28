list(
    APPEND LIBNUMSTORE_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_malloc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_os.c
)

add_subdirectory(faulty)
add_subdirectory(posix)
add_subdirectory(windows)

set(LIBNUMSTORE_SRCS ${LIBNUMSTORE_SRCS} PARENT_SCOPE)
