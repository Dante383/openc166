add_executable(openc166_tests
    unit/main.cpp
    unit/test_lexer.cpp
    unit/test_expr.cpp
    unit/test_encode.cpp
    unit/test_assembler.cpp
    unit/test_branches.cpp
    unit/test_defs.cpp
    unit/test_include.cpp)
target_include_directories(openc166_tests PRIVATE ${PROJECT_SOURCE_DIR}/third_party/doctest)
target_link_libraries(openc166_tests PRIVATE openc166::core openc166_options)
add_test(NAME unit COMMAND openc166_tests)

if(TARGET openc166_cli)
    add_test(NAME example
        COMMAND ${CMAKE_COMMAND}
            -DOPENC166=$<TARGET_FILE:openc166_cli>
            -DSOURCE=${PROJECT_SOURCE_DIR}/examples/payload.a66
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/payload.bin
            -P ${CMAKE_CURRENT_SOURCE_DIR}/example.cmake)
endif()
