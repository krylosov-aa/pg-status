cmake_minimum_required(VERSION 3.12)
project(pg-status C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

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

        "$<$<CONFIG:Debug>:-fsanitize=address,undefined>"
        "$<$<CONFIG:Debug>:-fno-omit-frame-pointer>"
        "$<$<CONFIG:Debug>:-g>"
)
target_link_options(common_warnings INTERFACE
        "$<$<CONFIG:Debug>:-fsanitize=address,undefined>"
        "$<$<CONFIG:Debug>:-fno-omit-frame-pointer>"
        "$<$<CONFIG:Debug>:-g3>"
)

find_package(PkgConfig REQUIRED)
add_subdirectory(src)
