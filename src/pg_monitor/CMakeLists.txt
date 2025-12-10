add_library(pg_monitor pg_monitor.c)


target_link_libraries(pg_monitor PUBLIC utils)

find_package(PkgConfig QUIET)

if(PKG_CONFIG_FOUND)
    pkg_check_modules(LIBPQ libpq QUIET)
endif()

if(NOT LIBPQ_FOUND)
    if(APPLE)
        set(HOMEBREW_PREFIX "/opt/homebrew") # ARM
        if(NOT EXISTS "${HOMEBREW_PREFIX}/opt/libpq/lib/libpq.dylib")
            set(HOMEBREW_PREFIX "/usr/local") # Intel
        endif()

        find_path(LIBPQ_INCLUDE_DIR libpq-fe.h
                HINTS
                ${HOMEBREW_PREFIX}/opt/libpq/include
                ${HOMEBREW_PREFIX}/include
                /usr/include
        )

        find_library(LIBPQ_LIBRARY pq
                HINTS
                ${HOMEBREW_PREFIX}/opt/libpq/lib
                ${HOMEBREW_PREFIX}/lib
                /usr/lib
        )
    endif()

    if(UNIX AND NOT APPLE)
        find_path(LIBPQ_INCLUDE_DIR libpq-fe.h
                HINTS
                /usr/include
                /usr/include/postgresql
                /usr/local/include
        )
        find_library(LIBPQ_LIBRARY pq
                HINTS
                /usr/lib
                /usr/lib/x86_64-linux-gnu
                /usr/local/lib
        )
    endif()

    if(LIBPQ_INCLUDE_DIR AND LIBPQ_LIBRARY)
        set(LIBPQ_FOUND TRUE)
    endif()
endif()

if(NOT LIBPQ_FOUND)
    message(FATAL_ERROR "libpq not found!")
endif()

target_include_directories(pg_monitor PRIVATE ${LIBPQ_INCLUDE_DIR})
target_link_libraries(pg_monitor PRIVATE ${LIBPQ_LIBRARY})


target_include_directories(pg_monitor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})