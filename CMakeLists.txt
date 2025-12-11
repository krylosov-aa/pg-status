cmake_minimum_required(VERSION 3.12)
project(pg-status C)

set(CMAKE_C_STANDARD 11)
add_compile_options(

        -Wall
        -Wextra
        -Wpedantic
#        -Werror
        -Wconversion
        -Wshadow
        -Wpointer-arith
        -Wstrict-prototypes
        -Wformat=2
        -Wundef
        -Wno-unused-parameter
)

add_subdirectory(src)
