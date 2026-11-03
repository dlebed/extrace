# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.25)
project(extrace VERSION 0.1.0 LANGUAGES C)

if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
    set(EXTRACE_TOP_LEVEL ON)
else()
    set(EXTRACE_TOP_LEVEL OFF)
endif()

option(EXTRACE_BUILD_TESTS "Build the unit, concurrency and lifecycle tests" ${EXTRACE_TOP_LEVEL})
set(EXTRACE_SANITIZE "" CACHE STRING "Sanitizers for test builds, e.g. address,undefined or thread")
option(EXTRACE_COVERAGE "Instrument test builds for coverage" OFF)
option(EXTRACE_WERROR "Treat warnings as errors" OFF)

include(cmake/extrace.cmake)

if(EXTRACE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
