list(
    APPEND LIBNUMSTORE_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_smartfiles_init_pager.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_smartfiles_insert.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_smartfiles_read.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_smartfiles_remove.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_smartfiles_size.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_smartfiles_write.c
)

set(LIBNUMSTORE_SRCS ${LIBNUMSTORE_SRCS} PARENT_SCOPE)
