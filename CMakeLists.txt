cmake_minimum_required(VERSION 3.20)
project(openc166
        VERSION 0.0.1
        DESCRIPTION "Keil A166-compatible assembler for the Infineon C166/C167 family"
        LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(OPENC166_TOP_LEVEL ON)
else()
    set(OPENC166_TOP_LEVEL OFF)
endif()

option(OPENC166_BUILD_CLI "Build the openc166 command-line tool" ON)
option(OPENC166_BUILD_TESTS "Build the unit tests" ${OPENC166_TOP_LEVEL})
option(OPENC166_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(OPENC166_WERROR "Treat compiler warnings as errors" OFF)
set(OPENC166_ISAGEN_EXECUTABLE "" CACHE FILEPATH
    "Prebuilt host isagen to use when cross-compiling (empty: build it)")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Shared warning and sanitizer settings for every target in this project.
add_library(openc166_options INTERFACE)
if(MSVC)
    target_compile_options(openc166_options INTERFACE /W4 /permissive- /utf-8)
    if(OPENC166_WERROR)
        target_compile_options(openc166_options INTERFACE /WX)
    endif()
else()
    target_compile_options(openc166_options INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)
    if(OPENC166_WERROR)
        target_compile_options(openc166_options INTERFACE -Werror)
    endif()
    if(OPENC166_SANITIZE)
        target_compile_options(openc166_options INTERFACE
            -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
        target_link_options(openc166_options INTERFACE -fsanitize=address,undefined)
    endif()
endif()

add_subdirectory(tools)
add_subdirectory(src)

if(OPENC166_BUILD_CLI)
    add_subdirectory(cli)
endif()

if(OPENC166_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
