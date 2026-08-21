/**
 * Various utilities for finding the right host for different conditions
 */

#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
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
 * Reads {status, lag_ms, lag_bytes, lsn} as a consistent snapshot
 * via the seqlock on host->seq. Spins until two reads of seq match and are
 * even (writer not in progress), so the returned values all come from the
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
    snap.lsn = atomic_load_explicit(&host->lsn, memory_order_relaxed);
    // Keep all snapshot loads before the final sequence validation.
    atomic_thread_fence(memory_order_acquire);
    const uint64_t seq2 = atomic_load_explicit(
      &host->seq, memory_order_relaxed
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
 * condition_handler that searches for a live replica. When the caller
 * supplies a LagThresholds ctx with a non-zero min_lsn, the replica must
 * also have replayed at least that LSN.
 */
bool is_alive_replica(
  const MonitorSnapshot snap, const MonitorHost *host, const void *ctx
) {
  if (!(snap.status.alive && !snap.status.master)) {
    return false;
  }
  if (ctx) {
    const LagThresholds *thresholds = ctx;
    if (thresholds->min_lsn != 0 && snap.lsn < thresholds->min_lsn) {
      return false;
    }
  }
  return true;
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
    !atomic_compare_exchange_weak(&round_robin_cursor, &cursor, new_cursor));

  return new_cursor;
}

/**
 * Searches for a replica host that matches the given condition using the
 * round-robin algorithm. Prefers a fully alive match; falls back to a
 * `possible_dead` match if no alive replica satisfies the handler. If no
 * replica matches at all, returns the current master as a fallback,
 * or nullptr if there is no master either.
 * @param handler A function that determines whether the specified host
 * matches
 * @param ctx Opaque context forwarded to the handler
 * @param log_context The context that will be visible in the logs
 * @return Host name matching the condition, or the master as a fallback, or
 * nullptr if no host is available
 */
const char *find_replica_round_robin(
  const condition_handler handler, const void *ctx, const char *log_context
) {
  if (host_count == 0) {
    return nullptr;
  }

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

  const char *master = get_master_host();
  if (master) {
    printf(
      "Master was returned instead of replica. Context: %s \n", log_context
    );
  }
  return master;
}

/**
 * Searches for the most byte-synchronous replica whose lag still satisfies
 * the byte threshold. Ties are broken by host order. Prefers a
 * fully alive match; falls back to a `possible_dead` match only if no
 * alive replica satisfies the thresholds. If no replica matches, returns
 * the current master, or nullptr if there is no master.
 * @param thresholds Lag thresholds the replica must satisfy
 * @param log_context The context that will be visible in the logs
 * @return Host name of the most byte-synchronous replica, or the master
 * as a fallback, or nullptr if no host is available
 */
const char *find_most_sync_replica_by_bytes(
  const LagThresholds *thresholds, const char *log_context
) {
  const MonitorHost *best_alive = nullptr;
  uint64_t best_alive_lag = UINT64_MAX;
  const MonitorHost *best_possible = nullptr;
  uint64_t best_possible_lag = UINT64_MAX;

  for (unsigned int i = 0; i < host_count; i++) {
    const MonitorHost *mon_host = &monitor_host_list[i];
    const MonitorSnapshot snap = atomic_get_snapshot(mon_host);

    if (!is_sync_replica_by_bytes(snap, mon_host, thresholds)) {
      continue;
    }

    if (snap.status.possible_dead) {
      if (snap.lag_bytes < best_possible_lag) {
        best_possible_lag = snap.lag_bytes;
        best_possible = mon_host;
      }
    } else {
      if (snap.lag_bytes < best_alive_lag) {
        best_alive_lag = snap.lag_bytes;
        best_alive = mon_host;
      }
    }
  }

  if (best_alive) {
    return best_alive->host;
  }
  if (best_possible) {
    return best_possible->host;
  }

  const char *master = get_master_host();
  if (master) {
    printf(
      "Master was returned instead of replica. Context: %s \n", log_context
    );
  }
  return master;
}
