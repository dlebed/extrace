# SPDX-License-Identifier: MIT
# The varint module alone (§18.1): no port, no catalog.
add_executable(test_varint test_varint.c varint_variants.c varint_chain.c)
target_include_directories(test_varint PRIVATE ${EXTRACE_ROOT}/include/extrace
                                               ${EXTRACE_ROOT}/port/common)
target_link_libraries(test_varint PRIVATE xt_harness)
target_compile_options(test_varint PRIVATE -Wall -Wextra -Wconversion)
add_test(NAME test_varint COMMAND test_varint)
