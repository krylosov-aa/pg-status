cmake_minimum_required(VERSION 3.12)
project(pg-status C)

set(CMAKE_C_STANDARD 23)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(SANITIZERS "-fsanitize=address,undefined -fno-omit-frame-pointer")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${SANITIZERS} -g")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${SANITIZERS}")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${SANITIZERS}")
endif()

add_compile_options(
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
)

find_package(PkgConfig REQUIRED)
add_subdirectory(src)
