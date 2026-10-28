add_executable(ns_dlread ns_dlread.c)
target_link_libraries(ns_dlread PRIVATE numstore)

add_executable(ns_nspprint ns_nspprint.c)
target_link_libraries(ns_nspprint PRIVATE numstore)

add_executable(ns_nsspprint ns_nsspprint.c)
target_link_libraries(ns_nsspprint PRIVATE numstore)

add_executable(ns_simtest ns_simtest.c)
target_link_libraries(ns_simtest PRIVATE numstore)

add_executable(ns_walpprint ns_walpprint.c)
target_link_libraries(ns_walpprint PRIVATE numstore)

add_executable(print_all_vars print_all_vars.c)
target_link_libraries(print_all_vars PRIVATE numstore)

add_executable(print_query print_query.c)
target_link_libraries(print_query PRIVATE numstore)

add_executable(print_type print_type.c)
target_link_libraries(print_type PRIVATE numstore)

add_executable(resolve_type_ref resolve_type_ref.c)
target_link_libraries(resolve_type_ref PRIVATE numstore)
