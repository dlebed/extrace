# SPDX-License-Identifier: MIT
# Runs an example and compares its output with the expected one.
# -DUPDATE=1 rewrites the expected file instead.
execute_process(COMMAND ${EXE} OUTPUT_VARIABLE out RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${EXE} exited with ${rc}\n${out}")
endif()
if(UPDATE)
    file(WRITE ${EXPECTED} "${out}")
    return()
endif()
file(READ ${EXPECTED} want)
if(NOT out STREQUAL want)
    file(WRITE ${EXPECTED}.actual "${out}")
    message(FATAL_ERROR "output differs from ${EXPECTED} (got ${EXPECTED}.actual)\n${out}")
endif()
