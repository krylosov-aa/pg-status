cmake_minimum_required(VERSION 3.12)
project(pg-status C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_program(CLANG_TIDY_EXE NAMES clang-tidy)

if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND CLANG_TIDY_EXE)
    set(CMAKE_C_CLANG_TIDY "${CLANG_TIDY_EXE}")
elseif(CMAKE_BUILD_TYPE STREQUAL "Debug")
    message(WARNING "clang-tidy not found; Debug builds will skip clang-tidy")
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

        "$<$<CONFIG:Debug>:-fsanitize=address,undefined>"
        "$<$<CONFIG:Debug>:-fno-omit-frame-pointer>"
        "$<$<CONFIG:Debug>:-g3>"
)
target_link_options(common_warnings INTERFACE
        "$<$<CONFIG:Debug>:-fsanitize=address,undefined>"
        "$<$<CONFIG:Debug>:-fno-omit-frame-pointer>"
        "$<$<CONFIG:Debug>:-g3>"
)

find_package(PkgConfig REQUIRED)
add_subdirectory(src)

file(GLOB_RECURSE FORMAT_SOURCES
        "${CMAKE_SOURCE_DIR}/src/*.c"
        "${CMAKE_SOURCE_DIR}/src/*.h"
)

file(GLOB_RECURSE LINT_SOURCES
        "${CMAKE_SOURCE_DIR}/src/*.c"
)

find_program(CLANG_FORMAT_EXE NAMES clang-format)
if(CLANG_FORMAT_EXE)
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
    elseif(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_dependencies(pg-status format-check)
    endif()
else()
    add_custom_target(format
            COMMAND ${CMAKE_COMMAND} -E echo "clang-format not found"
            COMMAND ${CMAKE_COMMAND} -E false
    )

    add_custom_target(format-check
            COMMAND ${CMAKE_COMMAND} -E echo "clang-format not found"
            COMMAND ${CMAKE_COMMAND} -E false
    )

    add_custom_target(format-warn
            COMMAND ${CMAKE_COMMAND} -E echo "clang-format not found"
            COMMAND ${CMAKE_COMMAND} -E false
    )
endif()

if(CLANG_TIDY_EXE)
    add_custom_target(lint
            COMMAND ${CLANG_TIDY_EXE} -p ${CMAKE_BINARY_DIR} ${LINT_SOURCES}
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            COMMENT "clang-tidy: checking sources"
    )
else()
    add_custom_target(lint
            COMMAND ${CMAKE_COMMAND} -E echo "clang-tidy not found"
            COMMAND ${CMAKE_COMMAND} -E false
    )
endif()
