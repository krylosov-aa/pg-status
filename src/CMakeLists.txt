add_subdirectory(utils)
add_subdirectory(http_server)
add_subdirectory(pg_monitor)

add_executable(main main.c)

target_link_libraries(main
        PRIVATE
        http_server
        utils
        pg_monitor
)

target_include_directories(main PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
