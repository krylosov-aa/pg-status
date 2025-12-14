cmake_minimum_required(VERSION 3.12)
project(pg-status C)

set(CMAKE_C_STANDARD 23)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address -g")
    set(CMAKE_LINKER_FLAGS "${CMAKE_LINKER_FLAGS} -fsanitize=address")
endif()

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
