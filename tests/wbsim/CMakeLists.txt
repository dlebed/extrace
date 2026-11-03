# SPDX-License-Identifier: MIT
# Write-buffer simulator runs (§18.3, §18.4): small rings and a small staging
# buffer, so wraps, carried-over records and restarts happen often.
xt_variant(xt_wbsim PORT wbsim DEFINES EXTRACE_CFG_MAX_PAYLOAD=64 EXTRACE_CFG_DISP_STAGE=96)
xt_test(test_lifecycle xt_wbsim test_lifecycle.c)
set_tests_properties(test_lifecycle PROPERTIES TIMEOUT 900)
xt_test(test_interleave xt_wbsim test_interleave.c)
set_tests_properties(test_interleave PROPERTIES TIMEOUT 900)
