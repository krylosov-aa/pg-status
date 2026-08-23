cmake_minimum_required(VERSION 3.22)
project(pg-status VERSION 2.1.1 LANGUAGES C)

if(NOT UNIX)
    message(FATAL_ERROR "pg-status currently supports POSIX systems only")
endif()

set(PG_STATUS_SUPPORTED_C_COMPILERS GNU Clang AppleClang)
if(NOT CMAKE_C_COMPILER_ID IN_LIST PG_STATUS_SUPPORTED_C_COMPILERS)
    message(FATAL_ERROR
            "Unsupported C compiler '${CMAKE_C_COMPILER_ID}'. "
            "Use GCC, Clang, or AppleClang."
    )
endif()

if(POLICY CMP0156)
    cmake_policy(SET CMP0156 NEW)
endif()
if(POLICY CMP0179)
    cmake_policy(SET CMP0179 NEW)
endif()

configure_file(
        src/pg_status_version.h.in
        ${CMAKE_CURRENT_BINARY_DIR}/pg_status_version.h
        @ONLY
)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

set(PG_STATUS_SANITIZER
        "none"
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
        OFF
)
option(
        PG_STATUS_STATIC_DEPENDENCIES
        "Resolve third-party dependencies as static libraries"
        OFF
)
option(
        PG_STATUS_LINK_WARNINGS_AS_ERRORS
        "Treat linker warnings as errors"
        ON
)

set(PG_STATUS_PKG_CONFIG_ARGN)
if(DEFINED PKG_CONFIG_ARGN)
    set(PG_STATUS_PKG_CONFIG_ARGN "${PKG_CONFIG_ARGN}")
endif()
# PG_STATUS_STATIC_DEPENDENCIES is the single source of truth for --static;
# remove a value cached by FindPkgConfig before applying the current profile.
list(REMOVE_ITEM PG_STATUS_PKG_CONFIG_ARGN --static)

if(PG_STATUS_STATIC_DEPENDENCIES)
    # Toolchain initialization may overwrite a command-line value of this
    # variable, so enforce it after project() has enabled the C compiler.
    set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")
    list(APPEND PG_STATUS_PKG_CONFIG_ARGN --static)
endif()
set(PKG_CONFIG_ARGN ${PG_STATUS_PKG_CONFIG_ARGN})

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

if(PG_STATUS_LINK_WARNINGS_AS_ERRORS)
    if(APPLE)
        target_link_options(common_warnings INTERFACE -Wl,-fatal_warnings)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_options(common_warnings INTERFACE -Wl,--fatal-warnings)
    endif()
endif()

if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    target_compile_options(common_warnings INTERFACE
            -Wmissing-variable-declarations
            -Wcomma
            -Wdocumentation
    )
endif()

set(PG_STATUS_TEST_ENVIRONMENT "")

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
message(STATUS
        "pg-status static dependencies: ${PG_STATUS_STATIC_DEPENDENCIES}"
)
message(STATUS
        "pg-status linker warnings as errors: "
        "${PG_STATUS_LINK_WARNINGS_AS_ERRORS}"
)

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)

if(PG_STATUS_STATIC_DEPENDENCIES)
    set(PG_STATUS_DEPENDENCY_PROFILE STATIC)
else()
    set(PG_STATUS_DEPENDENCY_PROFILE DYNAMIC)
endif()

set(PG_STATUS_CJSON_PKG_PREFIX
        "PG_STATUS_CJSON_${PG_STATUS_DEPENDENCY_PROFILE}"
)
pkg_check_modules(
        ${PG_STATUS_CJSON_PKG_PREFIX} REQUIRED IMPORTED_TARGET GLOBAL
        libcjson>=1.7
)
add_library(pg_status_cjson INTERFACE)
target_link_libraries(
        pg_status_cjson
        INTERFACE "PkgConfig::${PG_STATUS_CJSON_PKG_PREFIX}"
)

set(PG_STATUS_LIBEVENT_PKG_PREFIX
        "PG_STATUS_LIBEVENT_${PG_STATUS_DEPENDENCY_PROFILE}"
)
pkg_check_modules(
        ${PG_STATUS_LIBEVENT_PKG_PREFIX} REQUIRED IMPORTED_TARGET GLOBAL
        libevent>=2.0
)
add_library(pg_status_libevent INTERFACE)
target_link_libraries(
        pg_status_libevent
        INTERFACE "PkgConfig::${PG_STATUS_LIBEVENT_PKG_PREFIX}"
)

add_library(pg_status_libpq INTERFACE)
if((LIBPQ_INCLUDE_DIR AND NOT LIBPQ_LIBRARY) OR
        (LIBPQ_LIBRARY AND NOT LIBPQ_INCLUDE_DIR))
    message(FATAL_ERROR
            "LIBPQ_INCLUDE_DIR and LIBPQ_LIBRARY must be specified together"
    )
elseif(LIBPQ_INCLUDE_DIR AND LIBPQ_LIBRARY)
    target_include_directories(
            pg_status_libpq INTERFACE "${LIBPQ_INCLUDE_DIR}"
    )
    target_link_libraries(pg_status_libpq INTERFACE ${LIBPQ_LIBRARY})
else()
    set(PG_STATUS_LIBPQ_INCLUDE_HINTS "")
    set(PG_STATUS_LIBPQ_LIBRARY_HINTS "")

    # Homebrew's libpq is keg-only. Add its actual prefix so FindPkgConfig can
    # use libpq.pc, including private libraries in static builds.
    if(APPLE)
        find_program(PG_STATUS_BREW NAMES brew)
        if(PG_STATUS_BREW)
            execute_process(
                    COMMAND "${PG_STATUS_BREW}" --prefix libpq
                    OUTPUT_VARIABLE PG_STATUS_HOMEBREW_LIBPQ_PREFIX
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    RESULT_VARIABLE PG_STATUS_HOMEBREW_LIBPQ_RESULT
                    ERROR_QUIET
            )
            if(PG_STATUS_HOMEBREW_LIBPQ_RESULT EQUAL 0)
                list(APPEND CMAKE_PREFIX_PATH
                        "${PG_STATUS_HOMEBREW_LIBPQ_PREFIX}"
                )
                list(APPEND PG_STATUS_LIBPQ_INCLUDE_HINTS
                        "${PG_STATUS_HOMEBREW_LIBPQ_PREFIX}/include"
                )
                list(APPEND PG_STATUS_LIBPQ_LIBRARY_HINTS
                        "${PG_STATUS_HOMEBREW_LIBPQ_PREFIX}/lib"
                )
            endif()
        endif()
    endif()

    set(PG_STATUS_LIBPQ_PKG_PREFIX
            "PG_STATUS_LIBPQ_${PG_STATUS_DEPENDENCY_PROFILE}"
    )
    pkg_check_modules(
            ${PG_STATUS_LIBPQ_PKG_PREFIX}
            QUIET IMPORTED_TARGET GLOBAL
            libpq
    )
    if(TARGET "PkgConfig::${PG_STATUS_LIBPQ_PKG_PREFIX}")
        target_link_libraries(
                pg_status_libpq
                INTERFACE "PkgConfig::${PG_STATUS_LIBPQ_PKG_PREFIX}"
        )

        # Homebrew's static libpq currently references the public frontend
        # symbols from the *_shlib archives, while libpq.pc only lists the
        # regular pgcommon/pgport archives.  Add the matching archives after
        # libpq's pkg-config link line so those symbols can be resolved.
        if(APPLE AND PG_STATUS_STATIC_DEPENDENCIES AND
                PG_STATUS_HOMEBREW_LIBPQ_PREFIX)
            find_library(
                    PG_STATUS_HOMEBREW_PGCOMMON_SHLIB
                    NAMES pgcommon_shlib
                    HINTS "${PG_STATUS_HOMEBREW_LIBPQ_PREFIX}/lib"
                    NO_DEFAULT_PATH
            )
            find_library(
                    PG_STATUS_HOMEBREW_PGPORT_SHLIB
                    NAMES pgport_shlib
                    HINTS "${PG_STATUS_HOMEBREW_LIBPQ_PREFIX}/lib"
                    NO_DEFAULT_PATH
            )
            if(PG_STATUS_HOMEBREW_PGCOMMON_SHLIB)
                target_link_libraries(
                        pg_status_libpq
                        INTERFACE "${PG_STATUS_HOMEBREW_PGCOMMON_SHLIB}"
                )
            endif()
            if(PG_STATUS_HOMEBREW_PGPORT_SHLIB)
                target_link_libraries(
                        pg_status_libpq
                        INTERFACE "${PG_STATUS_HOMEBREW_PGPORT_SHLIB}"
                )
            endif()
        endif()
    else()
        find_program(PG_STATUS_PG_CONFIG NAMES pg_config)
        if(PG_STATUS_PG_CONFIG)
            execute_process(
                    COMMAND "${PG_STATUS_PG_CONFIG}" --includedir
                    OUTPUT_VARIABLE PG_STATUS_PG_CONFIG_INCLUDE_DIR
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    RESULT_VARIABLE PG_STATUS_PG_CONFIG_INCLUDE_RESULT
            )
            execute_process(
                    COMMAND "${PG_STATUS_PG_CONFIG}" --libdir
                    OUTPUT_VARIABLE PG_STATUS_PG_CONFIG_LIBRARY_DIR
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    RESULT_VARIABLE PG_STATUS_PG_CONFIG_LIBRARY_RESULT
            )
            if(PG_STATUS_PG_CONFIG_INCLUDE_RESULT EQUAL 0)
                list(APPEND PG_STATUS_LIBPQ_INCLUDE_HINTS
                        "${PG_STATUS_PG_CONFIG_INCLUDE_DIR}"
                )
            endif()
            if(PG_STATUS_PG_CONFIG_LIBRARY_RESULT EQUAL 0)
                list(APPEND PG_STATUS_LIBPQ_LIBRARY_HINTS
                        "${PG_STATUS_PG_CONFIG_LIBRARY_DIR}"
                )
            endif()
        endif()

        find_path(
                PG_STATUS_LIBPQ_INCLUDE_DIR
                NAMES libpq-fe.h
                HINTS ${PG_STATUS_LIBPQ_INCLUDE_HINTS}
                PATH_SUFFIXES postgresql
        )
        find_library(
                PG_STATUS_LIBPQ_LIBRARY
                NAMES pq libpq
                HINTS ${PG_STATUS_LIBPQ_LIBRARY_HINTS}
        )
        if(NOT PG_STATUS_LIBPQ_INCLUDE_DIR OR
                NOT PG_STATUS_LIBPQ_LIBRARY)
            message(FATAL_ERROR
                    "libpq not found; install its development package or set "
                    "LIBPQ_INCLUDE_DIR and LIBPQ_LIBRARY"
            )
        endif()
        target_include_directories(
                pg_status_libpq INTERFACE "${PG_STATUS_LIBPQ_INCLUDE_DIR}"
        )
        target_link_libraries(
                pg_status_libpq INTERFACE "${PG_STATUS_LIBPQ_LIBRARY}"
        )
    endif()
endif()

include(CTest)
add_subdirectory(src)

include(GNUInstallDirs)
install(TARGETS pg-status RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

if(BUILD_TESTING)
    add_subdirectory(test)
endif()

find_program(CLANG_FORMAT_EXE NAMES clang-format)

if(CLANG_FORMAT_EXE)
    file(GLOB_RECURSE FORMAT_SOURCES CONFIGURE_DEPENDS
            "${PROJECT_SOURCE_DIR}/src/*.c"
            "${PROJECT_SOURCE_DIR}/src/*.h"
            "${PROJECT_SOURCE_DIR}/test/*.c"
            "${PROJECT_SOURCE_DIR}/test/*.h"
    )
    list(FILTER FORMAT_SOURCES EXCLUDE REGEX "/\\.venv/")

    add_custom_target(format
            COMMAND "${CLANG_FORMAT_EXE}" -i ${FORMAT_SOURCES}
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
            COMMENT "clang-format: rewriting in place"
            COMMAND_EXPAND_LISTS
            VERBATIM
    )

    add_custom_target(format-check
            COMMAND "${CLANG_FORMAT_EXE}" --dry-run --Werror ${FORMAT_SOURCES}
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
            COMMENT "clang-format: strict dry-run (fails on diff)"
            COMMAND_EXPAND_LISTS
            VERBATIM
    )

    add_custom_target(format-warn
            COMMAND "${CLANG_FORMAT_EXE}" --dry-run ${FORMAT_SOURCES}
            WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
            COMMENT "clang-format: dry-run (warnings only)"
            COMMAND_EXPAND_LISTS
            VERBATIM
    )
endif()
