add_subdirectory(utils)
add_subdirectory(http_server)
add_subdirectory(pg_monitor)

add_executable(main main.c)

pkg_check_modules(CJSON REQUIRED IMPORTED_TARGET libcjson)

target_link_libraries(main
        PRIVATE
        utils
        http_server
        pg_monitor
        PkgConfig::CJSON
)

if(UNIX AND NOT APPLE)
    target_link_libraries(main PRIVATE pq microhttpd)
endif()

target_include_directories(main PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
