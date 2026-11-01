# Generate the instruction table from c166.tbl.
set(OPENC166_GENERATED_DIR ${PROJECT_BINARY_DIR}/generated)
set(_isa_tbl ${CMAKE_CURRENT_SOURCE_DIR}/isa/c166.tbl)
set(_isa_inc ${OPENC166_GENERATED_DIR}/openc166/isa_table.inc)
set(_isa_json ${OPENC166_GENERATED_DIR}/openc166/isa.json)

if(OPENC166_ISAGEN_EXECUTABLE)
    set(_isagen ${OPENC166_ISAGEN_EXECUTABLE})
    set(_isagen_dep "")
else()
    set(_isagen $<TARGET_FILE:isagen>)
    set(_isagen_dep isagen)
endif()

add_custom_command(
    OUTPUT ${_isa_inc} ${_isa_json}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${OPENC166_GENERATED_DIR}/openc166
    COMMAND ${_isagen} ${_isa_tbl} ${_isa_inc} ${_isa_json}
    DEPENDS ${_isa_tbl} ${_isagen_dep}
    COMMENT "Generating C166 instruction table"
    VERBATIM)

add_library(openc166_core STATIC
    ${_isa_inc}
    isa/isa.cpp
    assembler.cpp
    encode.cpp
    expr.cpp
    lexer.cpp
    operand.cpp
    predef.cpp)
add_library(openc166::core ALIAS openc166_core)

target_include_directories(openc166_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${OPENC166_GENERATED_DIR})
target_compile_features(openc166_core PUBLIC cxx_std_20)
target_link_libraries(openc166_core PRIVATE openc166_options)
set_target_properties(openc166_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
