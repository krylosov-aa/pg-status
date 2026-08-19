add_executable(http_server_test http_server_test.c)

target_link_libraries(
        http_server_test PRIVATE
        common_warnings
        http_server
)

add_test(NAME http_server_test COMMAND http_server_test)
set_tests_properties(http_server_test PROPERTIES TIMEOUT 15)
