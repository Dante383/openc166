add_executable(openc166_cli main.cpp)
set_target_properties(openc166_cli PROPERTIES OUTPUT_NAME openc166)
target_link_libraries(openc166_cli PRIVATE openc166::core openc166_options)
target_compile_definitions(openc166_cli PRIVATE OPENC166_VERSION="${PROJECT_VERSION}")
if(SKBUILD)
    # Python wheel: installed as a command next to the Python scripts.
    install(TARGETS openc166_cli RUNTIME DESTINATION ${SKBUILD_SCRIPTS_DIR})
else()
    install(TARGETS openc166_cli RUNTIME DESTINATION bin)
    # The C runtime and library, where the C166 clang looks for them: next
    # to bin/. Startup code and library modules per memory model, headers.
    install(FILES ${PROJECT_SOURCE_DIR}/runtime/cstart.a66
            DESTINATION share/openc166/runtime)
    foreach(dir lib tiny medium large)
        install(DIRECTORY ${PROJECT_SOURCE_DIR}/runtime/${dir}/
                DESTINATION share/openc166/runtime/${dir}
                FILES_MATCHING PATTERN "*.a66")
    endforeach()
    install(DIRECTORY ${PROJECT_SOURCE_DIR}/runtime/include/
            DESTINATION share/openc166/runtime/include
            FILES_MATCHING PATTERN "*.h")
endif()
