add_executable(openc166_tests
    unit/main.cpp
    unit/test_lexer.cpp
    unit/test_expr.cpp
    unit/test_encode.cpp
    unit/test_assembler.cpp
    unit/test_branches.cpp
    unit/test_defs.cpp
    unit/test_include.cpp
    unit/test_lit.cpp
    unit/test_cond.cpp
    unit/test_dpp.cpp
    unit/test_regbanks.cpp
    unit/test_output.cpp
    unit/test_sections.cpp
    unit/test_link.cpp
    unit/test_groups.cpp
    unit/test_bits.cpp
    unit/test_macros.cpp
    unit/test_mpl.cpp
    unit/test_tasking.cpp)
target_include_directories(openc166_tests PRIVATE ${PROJECT_SOURCE_DIR}/third_party/doctest)
# doctest's own declarations of std types break with newer MSVC libraries.
target_compile_definitions(openc166_tests PRIVATE DOCTEST_CONFIG_USE_STD_HEADERS)
target_link_libraries(openc166_tests PRIVATE openc166::core openc166_options)
add_test(NAME unit COMMAND openc166_tests)

if(TARGET openc166_shared)
    # The C interface, tested from C.
    enable_language(C)
    add_executable(openc166_capi_test capi/test_capi.c)
    set_target_properties(openc166_capi_test PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_link_libraries(openc166_capi_test PRIVATE openc166::shared openc166_options)
    if(WIN32)
        # Windows finds DLLs next to the program.
        add_custom_command(TARGET openc166_capi_test POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:openc166_shared> $<TARGET_FILE_DIR:openc166_capi_test>)
    endif()
    add_test(NAME capi COMMAND openc166_capi_test)

    # The Python package, run against the library in the build tree. A
    # sanitizer build cannot be loaded into an ordinary Python.
    find_package(Python3 3.9 COMPONENTS Interpreter)
    if(Python3_FOUND AND NOT OPENC166_SANITIZE)
        add_test(NAME python
            COMMAND ${CMAKE_COMMAND} -E env
                OPENC166_LIBRARY=$<TARGET_FILE:openc166_shared>
                PYTHONPATH=${PROJECT_SOURCE_DIR}/python
                PYTHONDONTWRITEBYTECODE=1
                ${Python3_EXECUTABLE} -m unittest discover -s ${PROJECT_SOURCE_DIR}/python/tests -v)
    endif()
endif()

if(TARGET openc166_cli)
    # Assembles an example and compares the binary byte for byte. Arguments
    # after OPTIONS are passed to openc166.
    function(add_binary_test name source)
        cmake_parse_arguments(PARSE_ARGV 2 arg "" "" "OPTIONS")
        list(JOIN arg_UNPARSED_ARGUMENTS "" expected)
        list(JOIN arg_OPTIONS "|" args)
        add_test(NAME ${name}
            COMMAND ${CMAKE_COMMAND}
                -DOPENC166=$<TARGET_FILE:openc166_cli>
                -DSOURCE=${PROJECT_SOURCE_DIR}/examples/${source}
                -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/${name}.bin
                -DEXPECTED=${expected}
                -DARGS=${args}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/check_binary.cmake)
    endfunction()

    # Assembles an example and compares the output file with one in
    # expected/. ARGN holds more options.
    function(add_output_test name source output expected)
        list(JOIN ARGN "|" args)
        add_test(NAME ${name}
            COMMAND ${CMAKE_COMMAND}
                -DOPENC166=$<TARGET_FILE:openc166_cli>
                -DSOURCE=${PROJECT_SOURCE_DIR}/examples/${source}
                -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/${output}
                -DEXPECTED_FILE=${CMAKE_CURRENT_SOURCE_DIR}/expected/${expected}
                -DARGS=${args}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/check_binary.cmake)
    endfunction()

    # Intel HEX and S-records, chosen by the output extension or --format.
    # They hold the same bytes as the binaries checked below.
    add_output_test(hex_output mtos/driver_patch.a66 driver_patch.hex driver_patch.hex)
    add_output_test(srec_s28_output mtos/driver_patch.a66 driver_patch.s28 driver_patch.s28)
    add_output_test(srec_format_option payload.a66 payload.out payload.srec --format srec)

    # The expected bytes match Keil A166 V5.38 linked with L166 and OH166.
    add_binary_test(example payload.a66
        "e6f214e0" "e6f300f6" "e084" "e832" "0832" "2841" "3dfc" "cb00"  # code
        "3412" "7856" "bc9a" "f0de" "4241" "4443" "0000" "ffff")         # Source

    # Sections without AT, placed by openc166 or by --section. The second
    # matches L166 with SECTIONS(PROG(0E000H),VARS(0F600H)).
    add_binary_test(sections_located sections.a66
        "f2f40ce0" "0841" "f6f40ce0" "cb00"
        OPTIONS --origin 0E000H)
    add_binary_test(sections_given sections.a66
        "f2f400f6" "0841" "f6f400f6" "cb00"
        OPTIONS --origin 0E000H --section VARS=0F600H)

    # The MTOS payloads of gkflasher. The expected bytes are the output of
    # flasher/mtos_asm.py in gkflasher, whose encodings were checked against
    # SIMK43 firmware. Keil A166 gives the same bytes.
    add_binary_test(mtos_marker mtos/marker.a66 "e6f4efbe" "f6f402c0" "db00")
    add_binary_test(mtos_kline_hello mtos/kline_hello.a66
        "6eb6"
        "e7584d00" "9ab6fe70" "7eb6"   # 'M'
        "e7585400" "9ab6fe70" "7eb6"   # 'T'
        "e7584f00" "9ab6fe70" "7eb6"   # 'O'
        "e7585300" "9ab6fe70" "7eb6"   # 'S'
        "e7580000" "9ab6fe70" "7eb6"   # dummy byte
        "6fb6" "db00")
    add_binary_test(mtos_memory_dump mtos/memory_dump.a66
        "6eb6" "e6f40001" "e6f54000"
        "a758a7a7" "d7400200" "a904" "f7f0b0fe" "9ab6fe70" "7eb6" "0841" "2851" "3df3"
        "e7580000" "9ab6fe70" "7eb6" "6fb6" "db00")
    add_binary_test(mtos_loader mtos/loader.a66
        "6eb7" "6eb6" "f3f0b2fe" "7eb7"
        "a758a7a7" "9ab7fc70" "f3f0b2fe" "7eb7" "c006"
        "a758a7a7" "9ab7fc70" "f3f0b2fe" "7eb7" "c007" "5c87" "0067" "e6f400e0"
        "a758a7a7" "9ab7fc70" "f3f0b2fe" "7eb7" "b904" "0841" "2861" "3df5"
        "da0000e0" "6fb7" "6fb6" "db00")
    add_binary_test(mtos_stage2_hello mtos/stage2_hello.a66
        "e758aa00" "9ab6fe70" "7eb6"
        "e7585500" "9ab6fe70" "7eb6"
        "e758c200" "9ab6fe70" "7eb6"
        "e7584f00" "9ab6fe70" "7eb6"
        "e7584b00" "9ab6fe70" "7eb6"
        "db00")
    add_binary_test(mtos_bsl_shim mtos/bsl_shim.a66
        "be88" "cc00" "e6d70100" "a758a7a7" "e60a00f2" "e60bfefd" "e60900f6" "cc00"
        "e60a00f3" "e60b00f6" "e60800f2" "cc00")
    string(REPEAT "ff" 204 driver_gap)  # 0F63AH..0F705H is not part of the patch
    add_binary_test(mtos_driver_patch mtos/driver_patch.a66
        "a758a7a7" "dc02" "a851" "cb00" ${driver_gap} "bb94" "cc00")

    # Rebuilds Infineon MiniMon from its TASKING sources and compares the
    # bytes with its HEX files. MiniMon is not part of openc166: the test
    # is skipped unless the environment variable MINIMON_DIR names it.
    find_package(Python3 3.9 COMPONENTS Interpreter)
    if(Python3_FOUND)
        add_test(NAME minimon_acceptance
            COMMAND ${CMAKE_COMMAND} -E env PYTHONDONTWRITEBYTECODE=1
                ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/acceptance/minimon.py
                --openc166 $<TARGET_FILE:openc166_cli>)
        set_tests_properties(minimon_acceptance PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()
