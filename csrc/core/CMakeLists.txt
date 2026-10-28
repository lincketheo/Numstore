list(
    APPEND LIBNUMSTORE_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_arena_alloc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_byte_accessor.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_cbuffer.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_crc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_data_validator.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_dbl_buffer.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_error.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_ext_array.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_gr_lock.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_htable.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_latch.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_linked_list.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_logging.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_numerics.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_periodic_task.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_robin_hood_ht.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_serial.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_slab_alloc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_stream.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_stride.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_string.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_testing.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_tests.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_utils.c
)

add_subdirectory(os)

set(LIBNUMSTORE_SRCS ${LIBNUMSTORE_SRCS} PARENT_SCOPE)
