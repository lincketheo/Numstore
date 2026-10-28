list(
    APPEND LIBNUMSTORE_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_parse_multi_user_stride.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_parse_query.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_parse_subtype.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_parse_type.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_parse_type_ref.c
    ${CMAKE_CURRENT_SOURCE_DIR}/ns_parse_user_stride.c
)

set(LIBNUMSTORE_SRCS ${LIBNUMSTORE_SRCS} PARENT_SCOPE)
