# SPDX-License-Identifier: MIT
#
# Producer API examples. They run on the host: the POSIX port stands in for the
# SoC, and a viewer in the same process reads the ring and prints every record.
#
#   cmake -S examples -B build-ex && cmake --build build-ex && ctest --test-dir build-ex
#
# Each example's output is compared with expected/<name>.txt (the clock is
# simulated, so the output is reproducible). The compile_fail/ cases must be
# rejected by the compiler with the given diagnostic.
cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(extrace_examples LANGUAGES C)
    include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/extrace.cmake)
    enable_testing()
endif()

set(EX_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(EX_POSIX_PORT ${EXTRACE_ROOT}/port/posix)

# The image: port, catalog and configuration of examples/config.
extrace_add_image(ex
    PORT_DIR   ${EX_POSIX_PORT}
    EVENTS_DIR ${EX_DIR}/config
    CFG_DIR    ${EX_DIR}/config
    COMPONENTS prod cons rec disp
    SOURCES    ${EX_POSIX_PORT}/extrace_port_posix.c)

add_library(ex_host STATIC common/ex_host.c)
target_include_directories(ex_host PUBLIC common)
target_link_libraries(ex_host PUBLIC ex::extrace)
set_target_properties(ex_host PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

set(EX_PROGRAMS
    01_quickstart
    02_ints
    03_printf
    04_payloads
    05_reserve
    06_control
    07_contexts
    08_ftrace)

foreach(ex IN LISTS EX_PROGRAMS)
    add_executable(${ex} ${ex}.c)
    target_link_libraries(${ex} PRIVATE ex_host)
    target_compile_options(${ex} PRIVATE -Wall -Wextra -Wconversion -Wshadow)
    set_target_properties(${ex} PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON
                                           C_EXTENSIONS OFF)
    add_test(NAME example_${ex}
             COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:${ex}>
                     -DEXPECTED=${EX_DIR}/expected/${ex}.txt -P ${EX_DIR}/run_example.cmake)
endforeach()

# Compile-time checks: each file must fail to compile with the diagnostic named
# in its first line ("expect: <regex>").
get_target_property(EX_INCS ex_extrace INTERFACE_INCLUDE_DIRECTORIES)
list(TRANSFORM EX_INCS PREPEND -I)
file(GLOB EX_FAIL_SRCS CONFIGURE_DEPENDS ${EX_DIR}/compile_fail/*.c)
foreach(src IN LISTS EX_FAIL_SRCS)
    get_filename_component(name ${src} NAME_WE)
    file(STRINGS ${src} expect REGEX "expect: " LIMIT_COUNT 1)
    string(REGEX REPLACE ".*expect: (.*)$" "\\1" expect "${expect}")
    add_test(NAME example_compile_fail_${name}
             COMMAND ${CMAKE_C_COMPILER} ${CMAKE_C23_STANDARD_COMPILE_OPTION} -fsyntax-only
                     ${EX_INCS} ${src})
    set_tests_properties(example_compile_fail_${name} PROPERTIES
                         PASS_REGULAR_EXPRESSION "${expect}")
endforeach()
