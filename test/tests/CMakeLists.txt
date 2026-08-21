add_library(http_test_support support/http_test.c)
target_link_libraries(http_test_support PRIVATE common_warnings)
target_include_directories(
        http_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/support
)

add_library(utils_test_support support/utils_test_support.c)
target_link_libraries(
        utils_test_support
        PUBLIC utils
        PRIVATE common_warnings
)
target_include_directories(
        utils_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/support
)

add_executable(utils_test test_utils.c)
target_link_libraries(
        utils_test PRIVATE common_warnings utils_test_support
)

set(UTILS_TEST_CASES
        printf_error
        copy_string
        concatenate_strings
        is_equal_strings
        format_string
        ulong_to_str
        long_to_str
        int_to_str
        uint_to_str
        str_to_long
        str_to_ulong
        str_to_ull
        try_str_to_ull_valid
        try_str_to_ull_invalid
        try_parse_lsn_valid
        try_parse_lsn_invalid
        format_lsn
        str_to_int
        str_to_int_greater_or_equal_zero
        str_to_uint
        str_to_uint16
        replace_from_env
        replace_from_env_uint
        replace_from_env_ull
        replace_from_env_copy
        replace_from_empty_env
        json_array
        json_object
        add_str_to_json_object
        add_null_to_json_object
        add_bool_to_json_object
        add_uint64_to_json_object
        json_to_str
        monotonic_ms
)

foreach(TEST_CASE IN LISTS UTILS_TEST_CASES)
    set(TEST_NAME "utils.${TEST_CASE}")
    add_test(NAME ${TEST_NAME} COMMAND utils_test ${TEST_CASE})
    set_tests_properties(
            ${TEST_NAME}
            PROPERTIES
            TIMEOUT 15
            ENVIRONMENT "${PG_STATUS_TEST_ENVIRONMENT}"
    )
endforeach()

function(add_utils_failure_test TEST_CASE EXPECTED_OUTPUT)
    set(TEST_NAME "utils.${TEST_CASE}")
    add_test(
            NAME ${TEST_NAME}
            COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:utils_test>"
            "-DARGUMENT=${TEST_CASE}"
            "-DEXPECTED_OUTPUT=${EXPECTED_OUTPUT}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/support/expect_failure.cmake
    )
    set_tests_properties(
            ${TEST_NAME}
            PROPERTIES
            TIMEOUT 15
            ENVIRONMENT "${PG_STATUS_TEST_ENVIRONMENT}"
    )
endfunction()

add_utils_failure_test(raise_error "Cannot open file")
add_utils_failure_test(str_to_long_null "Failed to convert null to long")
add_utils_failure_test(str_to_long_invalid "Failed to convert '42x' to long")
add_utils_failure_test(str_to_ulong_null "Failed to convert null to ulong")
add_utils_failure_test(str_to_ulong_negative "Failed to convert '-1' to ulong")
add_utils_failure_test(str_to_ull_null "Failed to convert null to ull")
add_utils_failure_test(
        str_to_ull_overflow
        "Failed to convert '18446744073709551616' to ull"
)
add_utils_failure_test(
        str_to_int_overflow
        "Failed to convert '2147483648' to int"
)
add_utils_failure_test(
        str_to_int_greater_or_equal_zero_negative
        "Failed to convert '-1' to int greater or equal zero"
)
add_utils_failure_test(
        str_to_uint_overflow
        "Failed to convert '4294967296' to uint"
)
add_utils_failure_test(
        str_to_uint16_overflow
        "Failed to convert '65536' to uint16"
)

add_test(
        NAME pg_status.invalid_port_list
        COMMAND ${CMAKE_COMMAND}
        "-DPROGRAM=$<TARGET_FILE:pg-status>"
        "-DEXPECTED_OUTPUT=pg_status__pg_port must contain at least one port"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/support/expect_failure.cmake
)
set_tests_properties(
        pg_status.invalid_port_list
        PROPERTIES
        LABELS functional
        TIMEOUT 15
        ENVIRONMENT
        "pg_status__hosts=localhost;pg_status__pg_port=,;${PG_STATUS_TEST_ENVIRONMENT}"
)

add_library(pg_status_test_support support/fixture_pg_status.c)
target_link_libraries(
        pg_status_test_support
        PUBLIC
        http_test_support
        pg_status_api
        pg_monitor
        PRIVATE
        common_warnings
        Threads::Threads
)
target_include_directories(
        pg_status_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/support
)

add_executable(http_server_test test_server.c)
target_link_libraries(
        http_server_test PRIVATE
        common_warnings
        http_server
        http_test_support
)

add_test(NAME http_server_test COMMAND http_server_test)
set_tests_properties(
        http_server_test
        PROPERTIES
        TIMEOUT 15
        ENVIRONMENT
        "${PG_STATUS_TEST_ENVIRONMENT}"
)

add_executable(pg_status_api_test test_pg_status_api.c)
target_link_libraries(
        pg_status_api_test PRIVATE
        common_warnings
        pg_status_test_support
)

set(PG_STATUS_API_TEST_CASES
        version
        version_ignores_accept
        master_text
        master_json
        replica_json
        hosts
        hosts_ignores_accept
        status_alive
        status_mixed_sync
        status_max_lsn
        status_ignores_accept
        snapshot_consistency
        status_dead
        missing_master_text
        missing_master_json
        missing_replica
        empty_topology_replica
        missing_replica_json
        missing_sync_by_time
        missing_sync_by_bytes
        missing_sync_or
        missing_sync_and
        missing_most_sync
        missing_status_host
        unknown_status_host
        unknown_route
        round_robin
        round_robin_master_in_middle
        replica_combined_lag_filters
        replica_lag_ms_fallback
        replica_lag_bytes_fallback
        replica_min_lsn_match
        replica_min_lsn_fallback
        sync_by_time_selects_candidate
        sync_by_bytes_falls_back
        sync_or_time_match
        sync_and_rejects_time_only
        sync_or_bytes_match
        sync_and_rejects_bytes_only
        sync_and_threshold_boundary
        sync_or_query_overrides_globals
        sync_and_query_overrides_globals
        sync_by_time_ignores_lag_bytes
        sync_by_bytes_ignores_lag_ms
        sync_by_time_min_lsn
        sync_by_bytes_min_lsn
        sync_or_min_lsn
        most_sync_by_bytes
        most_sync_lag_bytes_filter
        most_sync_min_lsn_filter
        most_sync_ignores_lag_ms
        replica_prefers_alive
        most_sync_prefers_alive
        replica_uses_possible_dead
        most_sync_uses_possible_dead
        most_sync_tie_break
        replica_lsn_boundary
        replica_high_lsn_boundary
        sync_and_lsn_boundary
        most_sync_lsn_boundary
        replica_lsn_above_boundary
        master_switch_master
        master_switch_replica
        master_switch_hosts
        invalid_parameters
)

foreach(TEST_CASE IN LISTS PG_STATUS_API_TEST_CASES)
    set(TEST_NAME "pg_status_api.${TEST_CASE}")
    add_test(NAME ${TEST_NAME} COMMAND pg_status_api_test ${TEST_CASE})
    set_tests_properties(
            ${TEST_NAME}
            PROPERTIES
            LABELS functional
            TIMEOUT 15
            ENVIRONMENT
            "${PG_STATUS_TEST_ENVIRONMENT}"
    )
endforeach()
