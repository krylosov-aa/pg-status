add_executable(fuzz_inputs fuzz_inputs.c)
target_link_libraries(fuzz_inputs PRIVATE common_warnings utils logging)
target_link_options(fuzz_inputs PRIVATE -fsanitize=fuzzer)

add_executable(fuzz_http fuzz_http.c)
target_link_libraries(fuzz_http PRIVATE common_warnings http_server logging)
target_link_options(fuzz_http PRIVATE -fsanitize=fuzzer)
