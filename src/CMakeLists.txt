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

# Build the files in inc/ (assembler include files) into the library.
set(_bundled ${PROJECT_SOURCE_DIR}/inc/REG167.INC)
set(_bundled_inc ${OPENC166_GENERATED_DIR}/openc166/bundled_files.inc)
string(REPLACE ";" "|" _bundled_arg "${_bundled}")
add_custom_command(
    OUTPUT ${_bundled_inc}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${OPENC166_GENERATED_DIR}/openc166
    COMMAND ${CMAKE_COMMAND} -DOUTPUT=${_bundled_inc} -DFILES=${_bundled_arg}
            -P ${PROJECT_SOURCE_DIR}/cmake/embed_files.cmake
    DEPENDS ${_bundled} ${PROJECT_SOURCE_DIR}/cmake/embed_files.cmake
    COMMENT "Embedding include files"
    VERBATIM)

add_library(openc166_core STATIC
    ${_isa_inc}
    ${_bundled_inc}
    isa/isa.cpp
    assembler.cpp
    bundled.cpp
    encode.cpp
    expr.cpp
    lexer.cpp
    m166.cpp
    operand.cpp
    output.cpp
    predef.cpp)
add_library(openc166::core ALIAS openc166_core)

target_include_directories(openc166_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${OPENC166_GENERATED_DIR})
target_compile_features(openc166_core PUBLIC cxx_std_20)
target_link_libraries(openc166_core PRIVATE openc166_options)
set_target_properties(openc166_core PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

# The C interface (include/openc166/openc166.h) as a shared library:
# libopenc166.so, libopenc166.dylib or openc166.dll. It exports only the
# oc166_* functions. The Python package loads it with ctypes.
if(OPENC166_BUILD_SHARED)
    add_library(openc166_shared SHARED capi.cpp)
    add_library(openc166::shared ALIAS openc166_shared)
    set_target_properties(openc166_shared PROPERTIES
        OUTPUT_NAME openc166
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
    target_include_directories(openc166_shared PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
    target_compile_definitions(openc166_shared PRIVATE
        OPENC166_BUILDING_LIBRARY
        OPENC166_VERSION="${PROJECT_VERSION}")
    target_link_libraries(openc166_shared PRIVATE openc166_core openc166_options)
    # Keep the C++ standard library code inside private, too.
    if(APPLE)
        target_link_options(openc166_shared PRIVATE "LINKER:-exported_symbol,_oc166_*")
    elseif(UNIX AND NOT CYGWIN)
        target_link_options(openc166_shared PRIVATE "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/capi.map")
        set_property(TARGET openc166_shared APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/capi.map)
    endif()

    if(SKBUILD)
        # Python wheel: the library goes into the openc166 package.
        install(FILES $<TARGET_FILE:openc166_shared> DESTINATION openc166)
    else()
        include(GNUInstallDirs)
        install(TARGETS openc166_shared
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
        install(FILES ${PROJECT_SOURCE_DIR}/include/openc166/openc166.h
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/openc166)
    endif()
endif()
