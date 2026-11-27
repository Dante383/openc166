add_executable(openc166_cli main.cpp)
set_target_properties(openc166_cli PROPERTIES OUTPUT_NAME openc166)
target_link_libraries(openc166_cli PRIVATE openc166::core openc166_options)
target_compile_definitions(openc166_cli PRIVATE OPENC166_VERSION="${PROJECT_VERSION}")
if(SKBUILD)
    # Python wheel: installed as a command next to the Python scripts.
    install(TARGETS openc166_cli RUNTIME DESTINATION ${SKBUILD_SCRIPTS_DIR})
else()
    install(TARGETS openc166_cli RUNTIME DESTINATION bin)
    # The C runtime, where the C166 clang looks for it: next to bin/.
    install(FILES ${PROJECT_SOURCE_DIR}/runtime/cstart.a66
            DESTINATION share/openc166/runtime)
    install(DIRECTORY ${PROJECT_SOURCE_DIR}/runtime/lib/
            DESTINATION share/openc166/runtime/lib
            FILES_MATCHING PATTERN "*.a66")
endif()
