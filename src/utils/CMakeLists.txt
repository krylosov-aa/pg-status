add_library(utils utils.c)

target_link_libraries(
        utils
        PUBLIC pg_status_cjson
        PRIVATE
        common_warnings
        logging
)

target_include_directories(utils PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
