cmake_minimum_required(VERSION 3.12)
project(pg-status VERSION 2.1.1 LANGUAGES C)

configure_file(
        src/pg_status_version.h.in
        ${CMAKE_CURRENT_BINARY_DIR}/pg_status_version.h
        @ONLY
)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_CLANG_TIDY "clang-tidy")
endif()

add_library(common_warnings INTERFACE)
target_compile_options(common_warnings INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
        -Wconversion
        -Wshadow
        -Wpointer-arith
        -Wstrict-prototypes
        -Wformat=2
        -Wundef
        -Wno-unused-parameter
        -Wmissing-field-initializers
        -Wmissing-prototypes
        -Wcast-align
        -Wvla
        -Walloca
        -Wimplicit-fallthrough
        -Wnull-dereference
        -Wdouble-promotion
        -Wswitch-enum
        -Wwrite-strings
        -Wcast-qual

        "$<$<CONFIG:Debug>:-fsanitize=address,undefined>"
        "$<$<CONFIG:Debug>:-fno-sanitize-recover=undefined>"
        "$<$<CONFIG:Debug>:-fsanitize-address-use-after-scope>"
        "$<$<CONFIG:Debug>:-fno-omit-frame-pointer>"
        "$<$<CONFIG:Debug>:-fno-optimize-sibling-calls>"
        "$<$<CONFIG:Debug>:-g3>"
)

if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    target_compile_options(common_warnings INTERFACE
            -Wmissing-variable-declarations
            -Wcomma
            -Wdocumentation

            "$<$<CONFIG:Debug>:-fno-sanitize-merge>"
    )
endif()

target_link_options(common_warnings INTERFACE
        "$<$<CONFIG:Debug>:-fsanitize=address,undefined>"
)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
include(CTest)
add_subdirectory(src)

if(BUILD_TESTING)
    add_subdirectory(test)
endif()

find_program(CLANG_FORMAT_EXE NAMES clang-format)

if(CLANG_FORMAT_EXE)
    file(GLOB_RECURSE FORMAT_SOURCES
            "${CMAKE_SOURCE_DIR}/src/*.c"
            "${CMAKE_SOURCE_DIR}/src/*.h"
            "${CMAKE_SOURCE_DIR}/test/*.c"
            "${CMAKE_SOURCE_DIR}/test/*.h"
    )

    add_custom_target(format
            COMMAND ${CLANG_FORMAT_EXE} -i ${FORMAT_SOURCES}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "clang-format: rewriting in place"
    )

    add_custom_target(format-check
            COMMAND ${CLANG_FORMAT_EXE} --dry-run --Werror ${FORMAT_SOURCES}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "clang-format: strict dry-run (fails on diff)"
    )

    add_custom_target(format-warn
            COMMAND ${CLANG_FORMAT_EXE} --dry-run ${FORMAT_SOURCES}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "clang-format: dry-run (warnings only)"
    )

    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_dependencies(pg-status format)
    endif()
endif()
