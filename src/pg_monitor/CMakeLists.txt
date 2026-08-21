add_library(pg_monitor
        check_utils.c
        pg_monitor.c
        lookup_utils.c
        host_list.c
        parameters.c
)

target_link_libraries(
        pg_monitor
        PUBLIC
        common_warnings
        utils
        PRIVATE
        logging
        Threads::Threads
)

if (PKG_CONFIG_FOUND)
    pkg_check_modules(LIBPQ libpq QUIET)
    if (LIBPQ_FOUND)
        if (NOT LIBPQ_INCLUDE_DIR)
            set(LIBPQ_INCLUDE_DIR ${LIBPQ_INCLUDE_DIRS})
        endif ()
        if (NOT LIBPQ_LIBRARY)
            set(LIBPQ_LIBRARY ${LIBPQ_LINK_LIBRARIES})
        endif ()
    endif ()
endif ()

if (NOT LIBPQ_FOUND)
    if (APPLE)
        set(HOMEBREW_PREFIX "/opt/homebrew") # ARM
        if (NOT EXISTS "${HOMEBREW_PREFIX}/opt/libpq/lib/libpq.dylib")
            set(HOMEBREW_PREFIX "/usr/local") # Intel
        endif ()

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
    endif ()

    if (UNIX AND NOT APPLE)
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
    endif ()

    if (LIBPQ_INCLUDE_DIR AND LIBPQ_LIBRARY)
        set(LIBPQ_FOUND TRUE)
    endif ()
endif ()

if (NOT LIBPQ_FOUND)
    message(FATAL_ERROR "libpq not found!")
endif ()

target_include_directories(pg_monitor PRIVATE ${LIBPQ_INCLUDE_DIR})
target_link_libraries(pg_monitor PRIVATE ${LIBPQ_LIBRARY})


target_include_directories(pg_monitor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
