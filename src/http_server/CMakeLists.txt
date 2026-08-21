add_library(http_server http_server.c)

target_link_libraries(
        http_server
        PRIVATE
        common_warnings
        utils
        pg_status_libevent
        logging
        Threads::Threads
)

target_include_directories(http_server PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
