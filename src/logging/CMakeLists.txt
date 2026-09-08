add_library(logging
        logger.c logger.h
        log_formatter.h text_formatter.c json_formatter.c
)

target_link_libraries(
        logging
        PRIVATE
        common_warnings
        Threads::Threads
)

target_include_directories(logging PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
