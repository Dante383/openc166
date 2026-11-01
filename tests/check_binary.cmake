# Assembles SOURCE and compares the binary with EXPECTED, a hex string.
# Usage: cmake -DOPENC166=... -DSOURCE=... -DOUTPUT=... -DEXPECTED=... -P check_binary.cmake
execute_process(
    COMMAND "${OPENC166}" "${SOURCE}" -o "${OUTPUT}"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE out)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "openc166 failed (exit status ${rc}):\n${out}")
endif()

file(READ "${OUTPUT}" actual HEX)
string(TOLOWER "${EXPECTED}" expected)
if(NOT actual STREQUAL expected)
    message(FATAL_ERROR "unexpected output\n  got:      ${actual}\n  expected: ${expected}")
endif()
