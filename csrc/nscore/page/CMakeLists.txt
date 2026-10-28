list(
    APPEND LIBNUMSTORE_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_page.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_page_data_list.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_page_fixture.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_page_fsm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_page_inner_node.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_page_var_hash_page.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_page_var_page.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_page_var_tail.c
)

set(LIBNUMSTORE_SRCS ${LIBNUMSTORE_SRCS} PARENT_SCOPE)
