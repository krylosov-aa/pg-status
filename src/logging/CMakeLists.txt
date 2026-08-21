add_library(logging logger.c logger.h)

target_link_libraries(
        logging
        PRIVATE
        common_warnings
        Threads::Threads
)

target_include_directories(logging PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
