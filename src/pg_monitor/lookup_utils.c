/**
 * Various utilities for finding the right host for different conditions
 */

#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>

#include "pg_monitor.h"
#include "utils.h"

/**
 * Atomically returns MonitorStatus
 */
MonitorStatus atomic_get_status(const MonitorHost *host) {
  return atomic_load_explicit(&host->status, memory_order_relaxed);
}

/**
 * Atomically returns the host's replication lag in milliseconds
 */
uint64_t atomic_get_lag_ms(const MonitorHost *host) {
  return atomic_load_explicit(&host->lag_ms, memory_order_relaxed);
}

/**
 * Atomically returns the host's replication lag in bytes
 */
uint64_t atomic_get_lag_bytes(const MonitorHost *host) {
  return atomic_load_explicit(&host->lag_bytes, memory_order_relaxed);
}

/**
 * Reads {status, lag_ms, lag_bytes} as a consistent snapshot via the
 * seqlock on host->seq. Spins until two reads of seq match and are even
 * (writer not in progress), so the returned values all come from the
 * same writer epoch.
 */
MonitorSnapshot atomic_get_snapshot(const MonitorHost *host) {
  MonitorSnapshot snap;
  for (;;) {
    const uint64_t seq1 = atomic_load_explicit(
      &host->seq, memory_order_acquire
    );
    if ((seq1 & 1U) != 0) {
      continue;  // writer in progress
    }
    snap.status = atomic_load_explicit(&host->status, memory_order_relaxed);
    snap.lag_ms = atomic_load_explicit(&host->lag_ms, memory_order_relaxed);
    snap.lag_bytes = atomic_load_explicit(
      &host->lag_bytes, memory_order_relaxed
    );
    const uint64_t seq2 = atomic_load_explicit(
      &host->seq, memory_order_acquire
    );
    if (seq1 == seq2) {
      return snap;
    }
    // writer advanced between the two seq reads; retry
  }
}

/**
 * Atomic acquisition of the current master
 */
const char *get_master_host(void) {
  const int master_i = get_master_index();
  if (master_i == -1) {
    return nullptr;
  }
  return monitor_host_list[master_i].host;
}

/**
 * A function for searching for a host by name
 */
const MonitorHost *find_host_by_name(const char *host) {
  for (unsigned int i = 0; i < host_count; i++) {
    const MonitorHost *item = &monitor_host_list[i];
    if (is_equal_strings(item->host, host)) {
      return item;
    }
  }
  return nullptr;
}

/**
 * condition_handler that searches for a live replica
 */
bool is_alive_replica(
  const MonitorSnapshot snap, const MonitorHost *host, const void *ctx
) {
  return snap.status.alive && !snap.status.master;
}

/**
 * condition_handler that searches for a live replica that is considered
 * time-synchronous
 */
bool is_sync_replica_by_time(
  const MonitorSnapshot snap, const MonitorHost *host, const void *ctx
) {
  if (!is_alive_replica(snap, host, ctx)) {
    return false;
  }
  const LagThresholds *thresholds = ctx;
  return snap.lag_ms <= thresholds->max_lag_ms;
}

/**
 * condition_handler that searches for a live replica that is considered
 * byte-synchronous
 */
bool is_sync_replica_by_bytes(
  const MonitorSnapshot snap, const MonitorHost *host, const void *ctx
) {
  if (!is_alive_replica(snap, host, ctx)) {
    return false;
  }
  const LagThresholds *thresholds = ctx;
  return snap.lag_bytes <= thresholds->max_lag_bytes;
}

/**
 * condition_handler that searches for a live replica that is considered
 * time-synchronous or byte-synchronous
 */
bool is_sync_replica_by_time_or_bytes(
  const MonitorSnapshot snap, const MonitorHost *host, const void *ctx
) {
  if (!is_alive_replica(snap, host, ctx)) {
    return false;
  }
  const LagThresholds *thresholds = ctx;
  return snap.lag_ms <= thresholds->max_lag_ms ||
         snap.lag_bytes <= thresholds->max_lag_bytes;
}

/**
 * condition_handler that searches for a live replica that is considered
 * time-synchronous and byte-synchronous
 */
bool is_sync_replica_by_time_and_bytes(
  const MonitorSnapshot snap, const MonitorHost *host, const void *ctx
) {
  if (!is_alive_replica(snap, host, ctx)) {
    return false;
  }
  const LagThresholds *thresholds = ctx;
  return snap.lag_ms <= thresholds->max_lag_ms &&
         snap.lag_bytes <= thresholds->max_lag_bytes;
}

/**
 * It takes the next host from the list, and if it's over,
 * it starts from the beginning.
 */
static unsigned int next_cursor_in_circle(const unsigned int cursor) {
  assert(cursor < host_count);
  return (cursor + 1) % host_count;
}

/**
 * A pointer to the last host returned in the round-robin algorithm
 */
static atomic_size_t round_robin_cursor = 0;

/**
 * Moves the cursor of the round-robin algorithm and returns the host from
 * which to start the crawl. Special logic to skip the master host so it
 * doesn't interfere with fair load balancing across the replicas.
 */
static unsigned int next_replica_round_robin(void) {
  size_t cursor;
  unsigned int new_cursor;
  do {
    cursor = atomic_load_explicit(&round_robin_cursor, memory_order_relaxed);
    new_cursor = (cursor + 1) % host_count;
    const int master_i = get_master_index();
    if (master_i != -1 && new_cursor == (unsigned int)master_i) {
      new_cursor = next_cursor_in_circle(new_cursor);
    }
  } while (
    !atomic_compare_exchange_weak(&round_robin_cursor, &cursor, new_cursor)
  );

  return new_cursor;
}

/**
 * A function for searching for a replica host that matches certain
 * conditions using the round-robin algorithm.
 * @param handler A function that determines whether the specified host
 * has been found
 * @param ctx Opaque context forwarded to the handler
 * @param log_context The context that will be visible in the logs
 * @return Host name corresponding to conditions
 */
const char *find_replica_round_robin(
  const condition_handler handler, const void *ctx, const char *log_context
) {
  unsigned int cursor = next_replica_round_robin();
  const unsigned int start_cursor = cursor;
  const MonitorHost *possible_mon_host = nullptr;

  do {
    const MonitorHost *mon_host = &monitor_host_list[cursor];
    const MonitorSnapshot snap = atomic_get_snapshot(mon_host);

    if (handler(snap, mon_host, ctx)) {
      if (!snap.status.possible_dead) {
        return mon_host->host;
      }
      if (!possible_mon_host) {
        possible_mon_host = mon_host;
      }
    }

    cursor = next_cursor_in_circle(cursor);
  } while (cursor != start_cursor);

  if (possible_mon_host) {
    return possible_mon_host->host;
  }

  printf("Master was returned instead of replica. Context: %s \n", log_context);
  return get_master_host();
}
