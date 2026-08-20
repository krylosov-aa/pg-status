add_subdirectory(utils)
add_subdirectory(http_server)
add_subdirectory(pg_monitor)

add_library(pg_status_api pg_status_api.c)
target_link_libraries(
        pg_status_api
        PUBLIC
        common_warnings
        http_server
        pg_monitor
        utils
)
target_include_directories(
        pg_status_api PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_BINARY_DIR}
)

add_executable(pg-status main.c)

pkg_check_modules(CJSON REQUIRED IMPORTED_TARGET libcjson)

target_link_libraries(pg-status
        PRIVATE
        common_warnings
        pg_status_api
        utils
        http_server
        pg_monitor
        PkgConfig::CJSON
        Threads::Threads
)

target_include_directories(pg-status PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
