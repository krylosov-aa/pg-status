add_subdirectory(utils)
add_subdirectory(http_server)
add_subdirectory(pg_monitor)

add_executable(pg-status main.c)

pkg_check_modules(CJSON REQUIRED IMPORTED_TARGET libcjson)

target_link_libraries(pg-status
        PRIVATE
        common_warnings
        utils
        http_server
        pg_monitor
        PkgConfig::CJSON
)

if(UNIX AND NOT APPLE)
    target_link_libraries(pg-status PRIVATE pq microhttpd)
endif()

target_include_directories(pg-status PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
