add_library(http_test_support support/http_test.c)
target_link_libraries(http_test_support PRIVATE common_warnings)
target_include_directories(
        http_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/support
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
set_tests_properties(http_server_test PROPERTIES TIMEOUT 15)

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
    )
endforeach()
