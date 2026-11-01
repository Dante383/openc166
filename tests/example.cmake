# Assembles examples/payload.a66 and compares the binary byte for byte.
# The expected bytes match Keil A166 V5.38 linked with L166 and OH166.
# Usage: cmake -DOPENC166=... -DSOURCE=... -DOUTPUT=... -P example.cmake
execute_process(
    COMMAND "${OPENC166}" "${SOURCE}" -o "${OUTPUT}"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE out)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "openc166 failed (exit status ${rc}):\n${out}")
endif()

file(READ "${OUTPUT}" actual HEX)
string(CONCAT expected
    "e6f214e0" "e6f300f6" "e084" "e832" "0832" "2841" "3dfc" "cb00"  # code
    "3412" "7856" "bc9a" "f0de" "4241" "4443" "0000" "ffff")         # Source
if(NOT actual STREQUAL expected)
    message(FATAL_ERROR "unexpected output\n  got:      ${actual}\n  expected: ${expected}")
endif()
