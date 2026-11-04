# Assembles SOURCE and compares the output with EXPECTED, a hex string of
# its bytes, or with the file EXPECTED_FILE. ARGS holds more options for
# openc166, separated by '|'.
# Usage: cmake -DOPENC166=... -DSOURCE=... -DOUTPUT=... -DEXPECTED=... [-DARGS=...] -P check_binary.cmake
string(REPLACE "|" ";" args "${ARGS}")
execute_process(
    COMMAND "${OPENC166}" "${SOURCE}" -o "${OUTPUT}" ${args}
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE out)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "openc166 failed (exit status ${rc}):\n${out}")
endif()

file(READ "${OUTPUT}" actual HEX)
if(EXPECTED_FILE)
    file(READ "${EXPECTED_FILE}" expected HEX)
else()
    string(TOLOWER "${EXPECTED}" expected)
endif()
if(NOT actual STREQUAL expected)
    message(FATAL_ERROR "unexpected output\n  got:      ${actual}\n  expected: ${expected}")
endif()
