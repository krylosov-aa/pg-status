if(NOT DEFINED PROGRAM OR NOT DEFINED EXPECTED_OUTPUT)
    message(FATAL_ERROR "PROGRAM and EXPECTED_OUTPUT must be specified")
endif()

set(COMMAND "${PROGRAM}")
if(DEFINED ARGUMENT)
    list(APPEND COMMAND "${ARGUMENT}")
endif()

execute_process(
        COMMAND ${COMMAND}
        RESULT_VARIABLE EXIT_CODE
        OUTPUT_VARIABLE STDOUT
        ERROR_VARIABLE STDERR
)

if("${EXIT_CODE}" STREQUAL "0")
    message(FATAL_ERROR "${PROGRAM} unexpectedly succeeded")
endif()

set(OUTPUT "${STDOUT}${STDERR}")
if(NOT OUTPUT MATCHES "${EXPECTED_OUTPUT}")
    message(FATAL_ERROR
            "Expected output '${EXPECTED_OUTPUT}', got:\n${OUTPUT}"
    )
endif()
