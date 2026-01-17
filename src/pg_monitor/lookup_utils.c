/**
 * Various utilities for finding the right host for different conditions
 */

#include "pg_monitor.h"
#include "utils.h"

#include <stdatomic.h>
#include <assert.h>


/**
 * Atomically returns MonitorStatus
 */
MonitorStatus atomic_get_status(const MonitorHost *host) {
    return atomic_load_explicit(
        &host -> status, memory_order_relaxed
    );
}

/**
 * Atomic acquisition of the current master
 */
const char *get_master_host(void) {
    return atomic_load_explicit(
        &master_host, memory_order_relaxed
    );
}

/**
 * A function for searching for a host by name
 */
const MonitorHost *find_host_by_name(const char *host) {
    for (uint8_t i = 0; i < host_count; i++) {
        MonitorHost *item = &monitor_host_list[i];
        if (is_equal_strings(item -> host, host)) {
            return item;
        }
    }
    return nullptr;
}

/**
 * condition_handler that searches for a live master
 */
bool is_master(const MonitorStatus status) {
    return status.alive && status.master;
}

/**
 * condition_handler that searches for a live replica
 */
bool is_alive_replica(const MonitorStatus status) {
    return status.alive && !status.master;
}

/**
 * condition_handler that searches for a live replica that is considered
 * time-synchronous
 */
bool is_sync_replica_by_time(const MonitorStatus status) {
    return is_alive_replica(status) && status.sync_by_time;
}

/**
 * condition_handler that searches for a live replica that is considered
 * byte-synchronous
 */
bool is_sync_replica_by_bytes(const MonitorStatus status) {
    return is_alive_replica(status)  && status.sync_by_bytes;
}

/**
 * condition_handler that searches for a live replica that is considered
 * time-synchronous or byte-synchronous
 */
bool is_sync_replica_by_time_or_bytes(const MonitorStatus status) {
    return (
        is_sync_replica_by_time(status) ||
        is_sync_replica_by_bytes(status)
    );
}

/**
 * condition_handler that searches for a live replica that is considered
 * time-synchronous and byte-synchronous
 */
bool is_sync_replica_by_time_and_bytes(const MonitorStatus status) {
    return (
        is_sync_replica_by_time(status) &&
        is_sync_replica_by_bytes(status)
    );
}

/**
 * It takes the next host from the list, and if it's over,
 * it starts from the beginning.
 */
static uint8_t get_next_cursor_in_circle(const uint8_t cursor) {
    assert(cursor < host_count);
    return (cursor + 1) % host_count;
}

/**
 * A pointer to the last host returned in the round-robin algorithm
 */
static atomic_size_t round_robin_cursor = 0;

/**
 * Moves the cursor of the round-robin algorithm and returns the host from
 * which to start the crawl
 */
static uint8_t get_next_cursor_round_robin(void) {
    return atomic_fetch_add_explicit(
        &round_robin_cursor, 1, memory_order_relaxed
    ) % host_count;
}

/**
 * A function for searching for a host that matches certain conditions using the round-robin algorithm.
 * @param handler A function that determines whether the specified host has been found
 * @param master_if_not_found Determines whether to return the master if the desired host is not found by handler
 * @return Host name corresponding to conditions
 */
const char *find_host_round_robin(
    const condition_handler handler, const bool master_if_not_found
) {
    uint8_t cursor = get_next_cursor_round_robin();
    const uint8_t start_cursor = cursor;

    const MonitorHost *mon_host = &monitor_host_list[cursor];
    MonitorStatus status = atomic_get_status(mon_host);

    while (!handler(status)) {
        cursor = get_next_cursor_in_circle(cursor);
        mon_host = &monitor_host_list[cursor];
        status = atomic_get_status(mon_host);

        if (cursor == start_cursor) {
            if (master_if_not_found) {
                return get_master_host();
            }
            return nullptr;
        }

    }

    return mon_host -> host;
}
