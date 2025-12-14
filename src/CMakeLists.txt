add_subdirectory(utils)
add_subdirectory(http_server)
add_subdirectory(pg_monitor)

add_executable(main main.c)

target_link_libraries(main
        PRIVATE
        utils
        http_server
        pg_monitor
)

target_include_directories(main PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
