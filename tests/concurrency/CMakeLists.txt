# SPDX-License-Identifier: MIT
# Real threads and a signal as the ISR (§18.2); also run under the tsan preset.
xt_test(test_concurrency xt_posix test_concurrency.c)
set_tests_properties(test_concurrency PROPERTIES TIMEOUT 600)
