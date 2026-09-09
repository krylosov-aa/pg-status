if(NOT DEFINED PROGRAM OR NOT DEFINED EXPECTED_OUTPUT)
    message(FATAL_ERROR "PROGRAM and EXPECTED_OUTPUT must be specified")
endif()

set(COMMAND "${PROGRAM}")
if(DEFINED ENV{PG_STATUS_TEST_VALGRIND} AND NOT "$ENV{PG_STATUS_TEST_VALGRIND}" STREQUAL "")
    separate_arguments(VALGRIND_OPTIONS UNIX_COMMAND "$ENV{PG_STATUS_TEST_VALGRIND_OPTIONS}")
    set(COMMAND "$ENV{PG_STATUS_TEST_VALGRIND}" ${VALGRIND_OPTIONS} "${PROGRAM}")
endif()
if(DEFINED ARGUMENT)
    list(APPEND COMMAND "${ARGUMENT}")
endif()

execute_process(
        COMMAND ${COMMAND}
        RESULT_VARIABLE EXIT_CODE
        OUTPUT_VARIABLE STDOUT
        ERROR_VARIABLE STDERR
)

if(NOT DEFINED EXPECTED_EXIT_CODE)
    set(EXPECTED_EXIT_CODE 1)
endif()
if(NOT "${EXIT_CODE}" STREQUAL "${EXPECTED_EXIT_CODE}")
    message(FATAL_ERROR
            "${PROGRAM}: expected exit ${EXPECTED_EXIT_CODE}, got ${EXIT_CODE}\n${STDOUT}${STDERR}")
endif()

set(OUTPUT "${STDOUT}${STDERR}")
if(OUTPUT MATCHES "AddressSanitizer|UndefinedBehaviorSanitizer|ThreadSanitizer|LeakSanitizer|runtime error:")
    message(FATAL_ERROR "Sanitizer diagnostic in expected-failure test:\n${OUTPUT}")
endif()
string(FIND "${OUTPUT}" "${EXPECTED_OUTPUT}" EXPECTED_OUTPUT_OFFSET)
if(EXPECTED_OUTPUT_OFFSET EQUAL -1)
    message(FATAL_ERROR
            "Expected output '${EXPECTED_OUTPUT}', got:\n${OUTPUT}"
    )
endif()
