cmake_minimum_required(VERSION 3.20)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/version.txt" NUMSTORE_VERSION)
string(STRIP "${NUMSTORE_VERSION}" NUMSTORE_VERSION)
string(REGEX REPLACE "^v" "" NUMSTORE_VERSION "${NUMSTORE_VERSION}")

project(numstore VERSION ${NUMSTORE_VERSION} LANGUAGES C)

include(GNUInstallDirs)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_definitions($<$<CONFIG:Release>:NLOG>)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

#####################################################################
### The numstore library
# csrc/ also holds the entry points below, so the library globs its two
# source trees rather than everything under csrc/.
file(
    GLOB_RECURSE NUMSTORE_SOURCES
    CONFIGURE_DEPENDS
    csrc/core/*.c
    csrc/nscore/*.c
)

# csrc/numstore.c is the public C API (include/numstore.h). pynumstore talks
# to nscore/nsdb directly, so a wheel build leaves it out entirely.
if(NOT SKBUILD)
    list(APPEND NUMSTORE_SOURCES csrc/numstore.c)
endif()

add_library(numstore STATIC ${NUMSTORE_SOURCES})
add_library(numstore::numstore ALIAS numstore)

target_include_directories(
    numstore
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/csrc>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
set_target_properties(numstore PROPERTIES POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    target_compile_options(numstore PUBLIC /experimental:c11atomics)
endif()

# Prefer using -pthread instead of -lpthread
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)
target_link_libraries(numstore PUBLIC Threads::Threads)

# long double / complex helpers are in libm on glibc.
if(UNIX AND NOT APPLE)
    target_link_libraries(numstore PUBLIC m)
endif()

# Install
install(
    TARGETS numstore
    EXPORT numstoreTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

#####################################################################
### pynumstore
### scikit-build-core defines SKBUILD, and a wheel build wants nothing else.
if(SKBUILD)
    find_package(
        Python
        3.9
        REQUIRED
        COMPONENTS Interpreter Development.Module NumPy
    )

    python_add_library(_pynumstore MODULE WITH_SOABI csrc/ns_pynumstore.c)
    target_link_libraries(_pynumstore PRIVATE numstore Python::NumPy)
    set_target_properties(_pynumstore PROPERTIES C_VISIBILITY_PRESET hidden)

    install(TARGETS _pynumstore LIBRARY DESTINATION pynumstore COMPONENT python)

    return()
endif()

#####################################################################
### ns_simtest
add_executable(ns_simtest apps/ns_simtest.c)
target_link_libraries(ns_simtest PRIVATE numstore)

#####################################################################
### samples
file(GLOB NUMSTORE_SAMPLES CONFIGURE_DEPENDS samples/*.c)

foreach(sample IN LISTS NUMSTORE_SAMPLES)
    get_filename_component(name ${sample} NAME_WE)
    add_executable(${name} ${sample})
    target_link_libraries(${name} PRIVATE numstore)
endforeach()

#####################################################################
### unit_tests
### gen_tests.py scrapes TEST(...) out of csrc/ and writes the main().
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    set(_unit_tests_c ${CMAKE_CURRENT_BINARY_DIR}/unit_tests.c)

    # Which os backend's tests to scrape. WIN32 is the target, not the host, so
    # this stays right under a mingw cross build.
    if(WIN32)
        set(_unit_tests_platform windows)
    else()
        set(_unit_tests_platform posix)
    endif()

    add_custom_command(
        OUTPUT ${_unit_tests_c}
        COMMAND
            ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/apps/gen_tests.py
            ${CMAKE_CURRENT_SOURCE_DIR}/apps/unit_tests.c.in ${_unit_tests_c}
            ${_unit_tests_platform}
        DEPENDS
            ${CMAKE_CURRENT_SOURCE_DIR}/apps/gen_tests.py
            ${CMAKE_CURRENT_SOURCE_DIR}/apps/unit_tests.c.in
            ${NUMSTORE_SOURCES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Generating unit_tests.c"
        VERBATIM
    )

    add_executable(unit_tests ${_unit_tests_c})
    target_link_libraries(unit_tests PRIVATE numstore)
endif()
