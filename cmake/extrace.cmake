# SPDX-License-Identifier: MIT
#
# Per-image integration (DESIGN §19.2). The library is compiled per image, so that
# the image's port header, event catalog and configuration apply and port hooks
# inline:
#
#   extrace_add_image(fw_a
#       PORT_DIR    ${CMAKE_CURRENT_SOURCE_DIR}/port        # extrace_port.h
#       EVENTS_DIR  ${CMAKE_CURRENT_SOURCE_DIR}/events      # extrace_events.def
#       CFG_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/cfg         # extrace_user_cfg.h (optional)
#       COMPONENTS  prod cons rec disp sink_mem sink_stats sink_text
#       DEFINES     EXTRACE_CFG_MAX_PAYLOAD=256             # optional
#       FTRACE_ARCH armv7m                                  # optional: armv7m or rv32
#       SOURCES     extra port sources)                     # optional
#   target_link_libraries(fw_a.elf PRIVATE fw_a::extrace)

set(EXTRACE_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)

# The library is never instrumented by -fpatchable-function-entry (docs/ftrace.md).
include(CheckCCompilerFlag)
check_c_compiler_flag(-fpatchable-function-entry=0 EXTRACE_HAS_PATCHABLE)

set(EXTRACE_WARNINGS
    -Wall -Wextra -Wconversion -Wshadow -Wcast-align -Wstrict-prototypes
    -Wmissing-prototypes -Wno-missing-field-initializers)

function(extrace_add_image name)
    cmake_parse_arguments(XT "" "PORT_DIR;EVENTS_DIR;CFG_DIR;FTRACE_ARCH"
                          "COMPONENTS;DEFINES;SOURCES" ${ARGN})
    set(srcs ${XT_SOURCES})
    if(XT_FTRACE_ARCH) # the trampoline is assembly: the project enables ASM
        set(adir ${EXTRACE_ROOT}/port/arch/${XT_FTRACE_ARCH})
        list(APPEND srcs ${adir}/extrace_ftrace_${XT_FTRACE_ARCH}.S
                         ${EXTRACE_ROOT}/src/extrace_ftrace.c)
        list(APPEND XT_DEFINES EXTRACE_CFG_FTRACE=1)
    endif()
    foreach(comp IN LISTS XT_COMPONENTS)
        if(comp STREQUAL "prod")
            list(APPEND srcs ${EXTRACE_ROOT}/src/extrace_prod.c)
        elseif(comp STREQUAL "cons")
            list(APPEND srcs ${EXTRACE_ROOT}/src/extrace_cons.c)
        elseif(comp STREQUAL "rec")
            list(APPEND srcs ${EXTRACE_ROOT}/src/extrace_rec.c)
        elseif(comp STREQUAL "disp")
            list(APPEND srcs ${EXTRACE_ROOT}/src/extrace_disp.c)
        elseif(comp STREQUAL "sink_mem")
            list(APPEND srcs ${EXTRACE_ROOT}/src/sinks/extrace_sink_mem.c)
        elseif(comp STREQUAL "sink_stats")
            list(APPEND srcs ${EXTRACE_ROOT}/src/sinks/extrace_sink_stats.c)
        elseif(comp STREQUAL "sink_text")
            list(APPEND srcs ${EXTRACE_ROOT}/src/sinks/extrace_sink_text.c)
        elseif(comp STREQUAL "test_hooks")
            list(APPEND srcs ${EXTRACE_ROOT}/src/extrace_test_hooks.c)
        else()
            message(FATAL_ERROR "extrace: unknown component '${comp}'")
        endif()
    endforeach()
    add_library(${name}_extrace STATIC ${srcs})
    add_library(${name}::extrace ALIAS ${name}_extrace)
    set(incs ${EXTRACE_ROOT}/include/extrace ${XT_PORT_DIR} ${EXTRACE_ROOT}/port/common)
    if(XT_EVENTS_DIR)
        list(APPEND incs ${XT_EVENTS_DIR})
    endif()
    if(XT_CFG_DIR)
        list(APPEND incs ${XT_CFG_DIR})
    endif()
    if(XT_FTRACE_ARCH)
        list(APPEND incs ${adir})
    endif()
    target_include_directories(${name}_extrace PUBLIC ${incs})
    target_compile_definitions(${name}_extrace PUBLIC ${XT_DEFINES})
    target_compile_options(${name}_extrace PRIVATE $<$<COMPILE_LANGUAGE:C>:${EXTRACE_WARNINGS}>)
    if(EXTRACE_HAS_PATCHABLE)
        target_compile_options(${name}_extrace PRIVATE
                               $<$<COMPILE_LANGUAGE:C>:-fpatchable-function-entry=0>)
    endif()
    if(EXTRACE_WERROR)
        target_compile_options(${name}_extrace PRIVATE -Werror)
    endif()
    set_target_properties(${name}_extrace PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON
                                                     C_EXTENSIONS OFF)
endfunction()
