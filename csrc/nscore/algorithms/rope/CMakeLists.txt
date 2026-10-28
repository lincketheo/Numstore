list(
    APPEND LIBNUMSTORE_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_balance_and_release.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_rope_dl_init.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_rope_dl_scan.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_rope_get_number_of_layers.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_rope_in_scan.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_rope_insert.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_rope_layer_scan.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_rope_page_stats.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_rope_read.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_rope_rebalance.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_rope_remove.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_rope_seek.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_rope_validate.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_rope_write.c
)

set(LIBNUMSTORE_SRCS ${LIBNUMSTORE_SRCS} PARENT_SCOPE)
