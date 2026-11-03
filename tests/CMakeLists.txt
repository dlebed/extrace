# SPDX-License-Identifier: MIT
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(EXTRACE_SANITIZE)
    add_compile_options(-fsanitize=${EXTRACE_SANITIZE} -fno-omit-frame-pointer
                        -fno-sanitize-recover=all)
    add_link_options(-fsanitize=${EXTRACE_SANITIZE})
endif()
if(EXTRACE_COVERAGE)
    add_compile_options(--coverage -O0)
    add_link_options(--coverage)
endif()

find_package(Threads REQUIRED)

set(XT_COMMON ${CMAKE_CURRENT_SOURCE_DIR}/common)
set(XT_POSIX_PORT ${EXTRACE_ROOT}/port/posix)
set(XT_WBSIM_PORT ${EXTRACE_ROOT}/port/wbsim)
set(XT_ALL_COMPONENTS prod cons rec disp sink_mem sink_stats sink_text test_hooks)

add_library(xt_harness STATIC harness/extrace_test.c)
target_include_directories(xt_harness PUBLIC harness)

# One library variant per port and configuration (§18.0 configuration matrix).
function(xt_variant name)
    cmake_parse_arguments(V "" "PORT" "DEFINES" ${ARGN})
    if(V_PORT STREQUAL "wbsim")
        set(pdir ${XT_WBSIM_PORT})
        set(psrc ${XT_WBSIM_PORT}/wbsim.c)
    else()
        set(pdir ${XT_POSIX_PORT})
        set(psrc ${XT_POSIX_PORT}/extrace_port_posix.c)
    endif()
    extrace_add_image(${name}
        PORT_DIR ${pdir}
        EVENTS_DIR ${XT_COMMON}
        CFG_DIR ${XT_COMMON}
        COMPONENTS ${XT_ALL_COMPONENTS}
        DEFINES EXTRACE_GRP_FORCED_ON=1 EXTRACE_CFG_FTRACE=1 ${V_DEFINES}
        SOURCES ${psrc})
    target_link_libraries(${name}_extrace PUBLIC Threads::Threads)
    if(EXTRACE_SANITIZE STREQUAL "thread")
        target_compile_definitions(${name}_extrace PUBLIC EXTRACE_POSIX_TSAN=1)
    endif()
endfunction()

function(xt_test name variant)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE ${variant}_extrace xt_harness)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/common
                                               ${CMAKE_CURRENT_SOURCE_DIR}/unit)
    target_compile_options(${name} PRIVATE -Wall -Wextra)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_subdirectory(varint)
add_subdirectory(unit)
add_subdirectory(concurrency)
add_subdirectory(wbsim)

# The ftrace trampolines of port/arch on an emulator: clang, ld.lld, Python unicorn.
find_program(XT_CLANG clang)
find_program(XT_LLD ld.lld)
find_package(Python3 COMPONENTS Interpreter)
set(xt_uc 1)
if(XT_CLANG AND XT_LLD AND Python3_Interpreter_FOUND)
    execute_process(COMMAND ${Python3_EXECUTABLE} -c "import unicorn" RESULT_VARIABLE xt_uc
                    OUTPUT_QUIET ERROR_QUIET)
endif()
if(xt_uc EQUAL 0)
    add_test(NAME ftrace_emu COMMAND ${Python3_EXECUTABLE}
             ${CMAKE_CURRENT_SOURCE_DIR}/ftrace/run_emu.py --clang ${XT_CLANG} --lld ${XT_LLD})
else()
    message(STATUS "extrace: ftrace emulator test skipped (needs clang, ld.lld, Python unicorn)")
endif()
# The same on QEMU, with the producer: the exact record bytes of every flavor.
find_program(XT_QEMU_RV32 qemu-system-riscv32)
find_program(XT_QEMU_ARM qemu-system-arm)
if(XT_CLANG AND XT_LLD AND Python3_Interpreter_FOUND AND XT_QEMU_RV32 AND XT_QEMU_ARM)
    add_test(NAME ftrace_qemu COMMAND ${Python3_EXECUTABLE}
             ${CMAKE_CURRENT_SOURCE_DIR}/ftrace/run_emu.py --emulator qemu
             --clang ${XT_CLANG} --lld ${XT_LLD})
    set_tests_properties(ftrace_qemu PROPERTIES TIMEOUT 600)
else()
    message(STATUS "extrace: ftrace QEMU test skipped (needs clang, ld.lld, qemu-system-riscv32, qemu-system-arm)")
endif()
