add_library(http_server http_server.c)

pkg_check_modules(MICROHTTPD REQUIRED IMPORTED_TARGET libmicrohttpd)
target_link_libraries(
        http_server PUBLIC
        utils
        PkgConfig::MICROHTTPD
)

target_include_directories(http_server PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
