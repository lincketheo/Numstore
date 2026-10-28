list(
    APPEND LIBNUMSTORE_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_numstore_create.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_numstore_delete.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_numstore_get.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_numstore_init_pager.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_numstore_insert.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_numstore_read.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_numstore_read_malloc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_numstore_remove.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_numstore_remove_malloc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_numstore_write.c
)

add_subdirectory(tests)

set(LIBNUMSTORE_SRCS ${LIBNUMSTORE_SRCS} PARENT_SCOPE)
