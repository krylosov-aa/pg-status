set(CASES normal crash diagnostic)
if(CHECK_MEMORY OR NOT "$ENV{PG_STATUS_TEST_VALGRIND}" STREQUAL "")
    list(APPEND CASES memory)
endif()
foreach(CASE IN LISTS CASES)
    execute_process(COMMAND "${CMAKE_COMMAND}"
            "-DPROGRAM=${PROBE}" "-DARGUMENT=${CASE}"
            "-DEXPECTED_OUTPUT=expected failure diagnostic"
            -P "${CMAKE_CURRENT_LIST_DIR}/expect_failure.cmake"
            RESULT_VARIABLE STATUS OUTPUT_VARIABLE OUTPUT ERROR_VARIABLE ERROR
            TIMEOUT 20)
    if(CASE STREQUAL "normal")
        if(NOT STATUS STREQUAL "0")
            message(FATAL_ERROR "Expected normal negative test to pass: ${OUTPUT}${ERROR}")
        endif()
    elseif(STATUS STREQUAL "0")
        message(FATAL_ERROR "Audit gate accepted an injected ${CASE} failure")
    endif()
endforeach()
