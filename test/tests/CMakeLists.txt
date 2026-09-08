add_library(http_test_support support/http_test.c)
target_link_libraries(http_test_support PRIVATE common_warnings)
target_include_directories(
        http_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/support
)

add_library(common_support support/common_support.c)
target_link_libraries(common_support PRIVATE common_warnings)
target_include_directories(
        common_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/support
)

add_executable(connection_test test_connection.c)
target_link_libraries(
        connection_test PRIVATE
        common_warnings
        common_support
        logging
        pg_monitor
        pg_status_libpq
)

set(CONNECTION_TEST_CASES
        separate_values
        dbname_not_expanded
        tls_environment
        secret_marker_not_logged
)

foreach(TEST_CASE IN LISTS CONNECTION_TEST_CASES)
    set(TEST_NAME "connection.${TEST_CASE}")
    add_test(NAME ${TEST_NAME} COMMAND connection_test ${TEST_CASE})
    set_tests_properties(
            ${TEST_NAME}
            PROPERTIES
            TIMEOUT 15
            ENVIRONMENT "${PG_STATUS_TEST_ENVIRONMENT}"
    )
endforeach()

add_executable(host_list_test test_host_list.c)
target_link_libraries(host_list_test PRIVATE common_warnings common_support pg_monitor)
foreach(TEST_CASE shared_port per_host_ports)
    add_test(NAME host_list.${TEST_CASE} COMMAND host_list_test ${TEST_CASE})
    set_tests_properties(host_list.${TEST_CASE} PROPERTIES
            TIMEOUT 15 ENVIRONMENT "${PG_STATUS_TEST_ENVIRONMENT}")
endforeach()

foreach(TEST_CASE
        empty_ports leading_empty_port middle_empty_port trailing_empty_port
        too_few_ports too_many_ports extra_port_single_host
        empty_hosts leading_empty_host middle_empty_host trailing_empty_host)
    if(TEST_CASE MATCHES "^(empty_hosts|leading_empty_host|middle_empty_host|trailing_empty_host)$")
        set(EXPECTED_OUTPUT "pg_status__hosts must not contain empty entries")
    elseif(TEST_CASE MATCHES "^(too_few_ports|too_many_ports|extra_port_single_host)$")
        set(EXPECTED_OUTPUT "pg_status__pg_port must contain one port or exactly one port per host")
    else()
        set(EXPECTED_OUTPUT "pg_status__pg_port must not contain empty entries")
    endif()
    add_test(NAME host_list.${TEST_CASE}
            COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:host_list_test>"
            "-DARGUMENT=${TEST_CASE}"
            "-DEXPECTED_OUTPUT=${EXPECTED_OUTPUT}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/support/expect_failure.cmake)
    set_tests_properties(host_list.${TEST_CASE} PROPERTIES
            TIMEOUT 15 ENVIRONMENT "${PG_STATUS_TEST_ENVIRONMENT}")
endforeach()

add_executable(locality_test test_locality.c)
target_link_libraries(
        locality_test PRIVATE
        common_warnings
        common_support
        logging
        pg_monitor
)

set(LOCALITY_TEST_CASES
        complete_direct_and_indirect
        no_locality_is_silent
        empty_locality_is_silent
        empty_direct_uses_indirect
        partial_dimensions_warn_independently
        empty_position_disables_only_dc
        wrong_cardinality_disables_dc
        extra_value_disables_geo
        unresolved_source_warns
        invalid_and_self_referencing_sources_warn
)

foreach(TEST_CASE IN LISTS LOCALITY_TEST_CASES)
    set(TEST_NAME "locality.${TEST_CASE}")
    add_test(NAME ${TEST_NAME} COMMAND locality_test ${TEST_CASE})
    set_tests_properties(
            ${TEST_NAME}
            PROPERTIES
            TIMEOUT 15
            ENVIRONMENT "${PG_STATUS_TEST_ENVIRONMENT}"
    )
endforeach()

add_executable(poll_result_test test_poll_result.c)
target_link_libraries(
        poll_result_test PRIVATE
        common_warnings
        common_support
        pg_monitor
        utils
        logging
        pg_status_libpq
)
target_include_directories(
        poll_result_test PRIVATE "${PROJECT_BINARY_DIR}/src/pg_monitor"
)
foreach(TEST_CASE
        latest_end_lsn
        latest_end_lsn_without_master
        older_latest_end_lsn
        missing_latest_end_lsn
        replay_ahead_of_observations
        master_ignores_latest_end_lsn
        permission_fallback_replica
        permission_fallback_master
        permission_fallback_failure
        permission_fallback_unrelated_error
        permission_fallback_timeout
        permission_fallback_send_failure)
    add_test(NAME poll_result.${TEST_CASE} COMMAND poll_result_test ${TEST_CASE})
    set_tests_properties(
            poll_result.${TEST_CASE}
            PROPERTIES
            TIMEOUT 15
            ENVIRONMENT "${PG_STATUS_TEST_ENVIRONMENT}"
    )
endforeach()

add_executable(poll_scheduler_test test_poll_scheduler.c)
target_link_libraries(poll_scheduler_test PRIVATE
        common_warnings common_support pg_monitor utils logging
        pg_status_libpq Threads::Threads)
target_include_directories(poll_scheduler_test PRIVATE "${PROJECT_BINARY_DIR}/src/pg_monitor")
foreach(TEST_CASE
        period overrun failure_period default_failure_timing
        independent_hosts_and_warmup deadline_before_event fresh_clock
        missing_socket wait_for_deadline defaults custom_parameters)
    add_test(NAME poll_scheduler.${TEST_CASE} COMMAND poll_scheduler_test ${TEST_CASE})
    set_tests_properties(poll_scheduler.${TEST_CASE} PROPERTIES
            TIMEOUT 15 ENVIRONMENT "${PG_STATUS_TEST_ENVIRONMENT}")
endforeach()
foreach(TEST_CASE zero_period zero_timeout overflow_timeout)
    if(TEST_CASE STREQUAL "zero_period")
        set(EXPECTED_OUTPUT "pg_status__sleep_ms must be greater than 0")
    else()
        set(EXPECTED_OUTPUT "pg_status__query_timeout_ms must be between 1 and")
    endif()
    add_test(NAME poll_scheduler.${TEST_CASE}
            COMMAND ${CMAKE_COMMAND}
            "-DPROGRAM=$<TARGET_FILE:poll_scheduler_test>"
            "-DARGUMENT=${TEST_CASE}"
            "-DEXPECTED_OUTPUT=${EXPECTED_OUTPUT}"
            -P ${CMAKE_CURRENT_SOURCE_DIR}/support/expect_failure.cmake)
    set_tests_properties(poll_scheduler.${TEST_CASE} PROPERTIES
            TIMEOUT 15 ENVIRONMENT "${PG_STATUS_TEST_ENVIRONMENT}")
endforeach()

add_executable(master_selection_test test_master_selection.c)
target_link_libraries(
        master_selection_test PRIVATE
        common_warnings
        common_support
        pg_monitor
        utils
        logging
        Threads::Threads
)
foreach(TEST_CASE
        healthy_priority
        possible_priority
        selected_master_becomes_replica
        selected_master_becomes_dead
        no_master
        dead_master_flags
        empty_hosts)
    add_test(NAME master_selection.${TEST_CASE} COMMAND master_selection_test ${TEST_CASE})
    set_tests_properties(
            master_selection.${TEST_CASE}
            PROPERTIES
            TIMEOUT 15
            ENVIRONMENT "${PG_STATUS_TEST_ENVIRONMENT}"
    )
endforeach()

add_executable(logger_test test_logger.c)
target_link_libraries(
        logger_test PRIVATE
        common_warnings
        logging
        pg_status_cjson
        common_support
        Threads::Threads
)

# Controlled formatter doubles verify that callers never execute formatting
# or retain caller-owned strings. Output tests below use the real formatters.
add_executable(logger_deferred_test test_logger_deferred.c ../../src/logging/logger.c)
target_include_directories(logger_deferred_test PRIVATE ../../src/logging)
target_link_libraries(logger_deferred_test PRIVATE common_warnings common_support Threads::Threads)
foreach(FORMAT IN ITEMS text json)
    add_test(NAME logger.deferred_${FORMAT} COMMAND logger_deferred_test ${FORMAT})
    set_tests_properties(logger.deferred_${FORMAT} PROPERTIES
            TIMEOUT 15 ENVIRONMENT "${PG_STATUS_TEST_ENVIRONMENT}")
endforeach()

set(LOGGER_TEST_CASES
        message_format
        text_format_environment
        json_format
        json_escape_roundtrip
        json_unicode_boundaries
        json_errno_boundaries
        json_exact_capacity
        json_shutdown_flushes
        json_concurrent_lifecycle
        json_truncation
        json_fitting_values_preserved
        json_retry_truncation
        json_utf8_truncation
        json_small_output_buffer
        json_invalid_utf8
        json_concurrent_messages
        json_backpressure_recovery
        json_partial_write_recovery
        json_failure_paths
        level_filtering
        init_from_environment
        system_error
        errno_preserved
        multiline_sanitized
        long_message_truncated
        long_component_truncated
        concurrent_messages
        ordered_messages
        shutdown_flushes
        concurrent_lifecycle
        closed_output
        missing_standard_error
        output_configuration_restored
        output_backpressure_recovery
        output_backpressure_periodic_recovery
        output_partial_write_recovery
        fatal_shutdown_race
)

foreach(TEST_CASE IN LISTS LOGGER_TEST_CASES)
    set(TEST_NAME "logger.${TEST_CASE}")
    add_test(NAME ${TEST_NAME} COMMAND logger_test ${TEST_CASE})
    set_tests_properties(
            ${TEST_NAME}
            PROPERTIES
            TIMEOUT 15
            ENVIRONMENT "${PG_STATUS_TEST_ENVIRONMENT}"
    )
endforeach()

add_test(
        NAME logger.invalid_format
        COMMAND ${CMAKE_COMMAND}
        "-DPROGRAM=$<TARGET_FILE:logger_test>"
        "-DARGUMENT=invalid_format"
        "-DEXPECTED_OUTPUT=invalid pg_status__log_format='yaml'"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/support/expect_failure.cmake
)
add_test(
        NAME logger.invalid_level
        COMMAND ${CMAKE_COMMAND}
        "-DPROGRAM=$<TARGET_FILE:logger_test>"
        "-DARGUMENT=invalid_level"
        "-DEXPECTED_OUTPUT=invalid pg_status__log_level='verbose'"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/support/expect_failure.cmake
)
add_test(
        NAME logger.fatal
        COMMAND ${CMAKE_COMMAND}
        "-DPROGRAM=$<TARGET_FILE:logger_test>"
        "-DARGUMENT=fatal"
        "-DEXPECTED_OUTPUT=FATAL test: fatal message value=42"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/support/expect_failure.cmake
)
add_test(
        NAME logger.fatal_flushes_queue
        COMMAND ${CMAKE_COMMAND}
        "-DPROGRAM=$<TARGET_FILE:logger_test>"
        "-DARGUMENT=fatal_flushes_queue"
        "-DEXPECTED_OUTPUT=INFO fatal: queued before fatal"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/support/expect_failure.cmake
)
add_test(
        NAME logger.invalid_enum_log
        COMMAND ${CMAKE_COMMAND}
        "-DPROGRAM=$<TARGET_FILE:logger_test>"
        "-DARGUMENT=invalid_enum_log"
        "-DEXPECTED_OUTPUT=invalid log level value=100"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/support/expect_failure.cmake
)
add_test(
        NAME logger.invalid_enum_set
        COMMAND ${CMAKE_COMMAND}
        "-DPROGRAM=$<TARGET_FILE:logger_test>"
        "-DARGUMENT=invalid_enum_set"
        "-DEXPECTED_OUTPUT=invalid log level value=100"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/support/expect_failure.cmake
)
set_tests_properties(
        logger.invalid_format
        logger.invalid_level
        logger.fatal
        logger.fatal_flushes_queue
        logger.invalid_enum_log
        logger.invalid_enum_set
        PROPERTIES
        TIMEOUT 15
        ENVIRONMENT "${PG_STATUS_TEST_ENVIRONMENT}"
)

add_executable(utils_test test_utils.c)
target_link_libraries(
        utils_test PRIVATE common_warnings common_support utils
)

set(UTILS_TEST_CASES
        copy_string
        is_equal_strings
        format_string
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

add_utils_failure_test(str_to_long_null "failed to convert null to long")
add_utils_failure_test(str_to_long_invalid "failed to convert '42x' to long")
add_utils_failure_test(str_to_ulong_null "failed to convert null to ulong")
add_utils_failure_test(str_to_ulong_negative "failed to convert '-1' to ulong")
add_utils_failure_test(str_to_ull_null "failed to convert null to ull")
add_utils_failure_test(
        str_to_ull_overflow
        "failed to convert '18446744073709551616' to ull"
)
add_utils_failure_test(
        str_to_int_overflow
        "failed to convert '2147483648' to int"
)
add_utils_failure_test(
        str_to_int_greater_or_equal_zero_negative
        "failed to convert '-1' to int greater or equal zero"
)
add_utils_failure_test(
        str_to_uint_overflow
        "failed to convert '4294967296' to uint"
)
add_utils_failure_test(
        str_to_uint16_overflow
        "failed to convert '65536' to uint16"
)

add_test(
        NAME pg_status.invalid_port_list
        COMMAND ${CMAKE_COMMAND}
        "-DPROGRAM=$<TARGET_FILE:pg-status>"
        "-DEXPECTED_OUTPUT=pg_status__pg_port must not contain empty entries"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/support/expect_failure.cmake
)
set_tests_properties(
        pg_status.invalid_port_list
        PROPERTIES
        LABELS functional
        TIMEOUT 15
        ENVIRONMENT
        "pg_status__hosts=localhost;pg_status__pg_port=,;pg_status__http_port=0;${PG_STATUS_TEST_ENVIRONMENT}"
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
        pg_status_cjson
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
        utils
)

add_executable(startup_test test_startup.c)
target_link_libraries(
        startup_test PRIVATE
        common_warnings
        common_support
        http_test_support
        pg_status_api
        pg_monitor
        utils
)
foreach(TEST_CASE readiness shutdown_during_warmup)
    add_test(NAME startup.${TEST_CASE} COMMAND startup_test ${TEST_CASE})
    set_tests_properties(
            startup.${TEST_CASE}
            PROPERTIES
            LABELS functional
            TIMEOUT 15
            ENVIRONMENT "${PG_STATUS_TEST_ENVIRONMENT}"
    )
endforeach()

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
        status_possible_dead
        poll_failure_preserves_measurements
        poll_failure_does_not_promote_lagging_replica
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
        round_robin_filtered_hosts
        round_robin_locality_groups
        round_robin_health_groups
        round_robin_topology_changes
        round_robin_concurrent
        locality_prefers_dc_over_geo
        locality_uses_geo_when_dc_candidate_is_ineligible
        locality_incomplete_dc_still_uses_geo
        locality_falls_back_to_round_robin
        locality_preserves_alive_priority
        most_sync_ignores_locality
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
