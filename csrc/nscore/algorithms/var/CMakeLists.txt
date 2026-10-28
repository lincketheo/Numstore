list(
    APPEND LIBNUMSTORE_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_find_var_page.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_init_var_hash_map.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_read_var_page.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_var_create.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_var_delete.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_var_get.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_var_get_or_create.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_var_update.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_visit_variables.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_write_var_page.c
)

set(LIBNUMSTORE_SRCS ${LIBNUMSTORE_SRCS} PARENT_SCOPE)
