cmake_minimum_required(VERSION 3.12)
project(pg-status VERSION 2.1.1 LANGUAGES C)

configure_file(
        src/pg_status_version.h.in
        ${CMAKE_CURRENT_BINARY_DIR}/pg_status_version.h
        @ONLY
)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(PG_STATUS_DEFAULT_SANITIZER "none")
set(PG_STATUS_DEFAULT_CLANG_TIDY OFF)
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(PG_STATUS_DEFAULT_SANITIZER "address-undefined")
    set(PG_STATUS_DEFAULT_CLANG_TIDY ON)
endif()

set(PG_STATUS_SANITIZER
        "${PG_STATUS_DEFAULT_SANITIZER}"
        CACHE STRING
        "Runtime sanitizer: none, address-undefined, or thread"
)
set_property(
        CACHE PG_STATUS_SANITIZER
        PROPERTY STRINGS none address-undefined thread
)

if(NOT PG_STATUS_SANITIZER STREQUAL "none" AND
        NOT PG_STATUS_SANITIZER STREQUAL "address-undefined" AND
        NOT PG_STATUS_SANITIZER STREQUAL "thread")
    message(FATAL_ERROR
            "Invalid PG_STATUS_SANITIZER='${PG_STATUS_SANITIZER}'. "
            "Expected none, address-undefined, or thread."
    )
endif()

option(
        PG_STATUS_ENABLE_CLANG_TIDY
        "Run clang-tidy while compiling"
        ${PG_STATUS_DEFAULT_CLANG_TIDY}
)

if(PG_STATUS_ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY_EXE NAMES clang-tidy)
    if(NOT CLANG_TIDY_EXE)
        message(FATAL_ERROR
                "PG_STATUS_ENABLE_CLANG_TIDY requires clang-tidy"
        )
    endif()
    set(CMAKE_C_CLANG_TIDY "${CLANG_TIDY_EXE}")
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

        "$<$<CONFIG:Debug>:-g3>"
)

if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    target_compile_options(common_warnings INTERFACE
            -Wmissing-variable-declarations
            -Wcomma
            -Wdocumentation
    )
endif()

set(PG_STATUS_TEST_ENVIRONMENT)

if(PG_STATUS_SANITIZER STREQUAL "address-undefined")
    target_compile_options(common_warnings INTERFACE
            -fsanitize=address,undefined
            -fno-sanitize-recover=undefined
            -fsanitize-address-use-after-scope
            -fno-omit-frame-pointer
            -fno-optimize-sibling-calls
    )
    target_link_options(common_warnings INTERFACE
            -fsanitize=address,undefined
    )

    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        include(CheckCCompilerFlag)
        check_c_compiler_flag(
                "-fno-sanitize-merge"
                PG_STATUS_HAS_FNO_SANITIZE_MERGE
        )
        if(PG_STATUS_HAS_FNO_SANITIZE_MERGE)
            target_compile_options(common_warnings INTERFACE
                    -fno-sanitize-merge
            )
        endif()
    endif()

    set(PG_STATUS_TEST_ENVIRONMENT
            "ASAN_OPTIONS=abort_on_error=1:strict_string_checks=1:detect_stack_use_after_return=1"
            "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1"
    )
elseif(PG_STATUS_SANITIZER STREQUAL "thread")
    target_compile_options(common_warnings INTERFACE
            -fsanitize=thread
            -O1
            -g
            -fno-omit-frame-pointer
            -fno-optimize-sibling-calls
    )
    target_link_options(common_warnings INTERFACE
            -fsanitize=thread
    )

    set(PG_STATUS_TEST_ENVIRONMENT
            "TSAN_OPTIONS=halt_on_error=1"
    )
endif()

message(STATUS "pg-status sanitizer: ${PG_STATUS_SANITIZER}")
message(STATUS "pg-status clang-tidy: ${PG_STATUS_ENABLE_CLANG_TIDY}")

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
