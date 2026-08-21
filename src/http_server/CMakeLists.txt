add_library(http_server http_server.c)

pkg_check_modules(
        LIBEVENT REQUIRED IMPORTED_TARGET
        libevent
)
target_link_libraries(
        http_server
        PUBLIC
        common_warnings
        utils
        PkgConfig::LIBEVENT
        PRIVATE
        logging
        Threads::Threads
)

target_include_directories(http_server PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
