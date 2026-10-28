list(
    APPEND LIBNUMSTORE_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/0001_create_delete_rollback_delete.c
    ${CMAKE_CURRENT_SOURCE_DIR}/0002_create_crash_close_delete.c
    ${CMAKE_CURRENT_SOURCE_DIR}/0003_rollback_invald_wal_header.c
    ${CMAKE_CURRENT_SOURCE_DIR}/0004_crash_after_churn_diverges.c
)

set(LIBNUMSTORE_SRCS ${LIBNUMSTORE_SRCS} PARENT_SCOPE)
