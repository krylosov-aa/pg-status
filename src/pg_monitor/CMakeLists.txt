add_library(pg_monitor
        check_utils.c
        pg_monitor.c
        lookup_utils.c
        host_list.c
        parameters.c
)

set_property(
        DIRECTORY APPEND
        PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/streaming_replication_query.sql"
)
file(
        READ
        "${CMAKE_CURRENT_SOURCE_DIR}/streaming_replication_query.sql"
        PG_STATUS_STREAMING_REPLICATION_QUERY_HEX
        HEX
)
string(
        REGEX REPLACE
        "(..)" "0x\\1, "
        PG_STATUS_STREAMING_REPLICATION_QUERY_BYTES
        "${PG_STATUS_STREAMING_REPLICATION_QUERY_HEX}"
)
configure_file(
        streaming_replication_query.h.in
        "${CMAKE_CURRENT_BINARY_DIR}/streaming_replication_query.h"
        @ONLY
)

target_link_libraries(
        pg_monitor
        PRIVATE
        common_warnings
        utils
        logging
        pg_status_libpq
        Threads::Threads
)

target_include_directories(
        pg_monitor
        PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
        PRIVATE "${CMAKE_CURRENT_BINARY_DIR}"
)
