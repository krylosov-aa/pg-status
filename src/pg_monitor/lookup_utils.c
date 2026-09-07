/**
 * Various utilities for finding the right host for different conditions
 */

#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>

#include "logger.h"
#include "pg_monitor.h"
#include "utils.h"

MonitorStatus atomic_get_status(const MonitorHost *host) {
  return atomic_load_explicit(&host->status, memory_order_relaxed);
}

uint64_t atomic_get_lag_ms(const MonitorHost *host) {
  return atomic_load_explicit(&host->lag_ms, memory_order_relaxed);
}

uint64_t atomic_get_lag_bytes(const MonitorHost *host) {
  return atomic_load_explicit(&host->lag_bytes, memory_order_relaxed);
}

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

const MonitorHost *get_master_monitor_host(void) {
  const int master_i = get_master_index();
  if (master_i == -1) {
    return nullptr;
  }
  return &monitor_host_list[master_i];
}

const MonitorHost *find_host_by_name(const char *host) {
  for (unsigned int i = 0; i < host_count; i++) {
    const MonitorHost *item = &monitor_host_list[i];
    if (is_equal_strings(item->host, host)) {
      return item;
    }
  }
  return nullptr;
}

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

bool is_sync_replica_by_time(
  const MonitorSnapshot snap, const MonitorHost *host, const void *ctx
) {
  if (!is_alive_replica(snap, host, ctx)) {
    return false;
  }
  const LagThresholds *thresholds = ctx;
  return snap.lag_ms <= thresholds->max_lag_ms;
}

bool is_sync_replica_by_bytes(
  const MonitorSnapshot snap, const MonitorHost *host, const void *ctx
) {
  if (!is_alive_replica(snap, host, ctx)) {
    return false;
  }
  const LagThresholds *thresholds = ctx;
  return snap.lag_bytes <= thresholds->max_lag_bytes;
}

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
    new_cursor = (unsigned int)((cursor + 1) % host_count);
    const int master_i = get_master_index();
    if (master_i != -1 && new_cursor == (unsigned int)master_i) {
      new_cursor = next_cursor_in_circle(new_cursor);
    }
  } while (
    !atomic_compare_exchange_weak(&round_robin_cursor, &cursor, new_cursor));

  return new_cursor;
}

enum {
  LOCALITY_DC = 0,
  LOCALITY_GEO = 1,
  LOCALITY_ANY = 2,
  LOCALITY_COUNT = 3,
};

static unsigned int locality_rank(const MonitorHost *host) {
  if (
    parameters.dc_locality_enabled &&
    is_equal_strings(host->dc, parameters.current_dc)
  ) {
    return LOCALITY_DC;
  }
  if (
    parameters.geo_locality_enabled &&
    is_equal_strings(host->geo, parameters.current_geo)
  ) {
    return LOCALITY_GEO;
  }
  return LOCALITY_ANY;
}

static const MonitorHost *find_replica_round_robin_plain(
  const condition_handler handler, const void *ctx
) {
  const MonitorHost *possible = nullptr;
  unsigned int cursor = next_replica_round_robin();
  const unsigned int start_cursor = cursor;

  do {
    const MonitorHost *mon_host = &monitor_host_list[cursor];
    const MonitorSnapshot snap = atomic_get_snapshot(mon_host);

    if (handler(snap, mon_host, ctx)) {
      if (!snap.status.possible_dead) {
        return mon_host;
      }
      if (!possible) {
        possible = mon_host;
      }
    }

    cursor = next_cursor_in_circle(cursor);
  } while (cursor != start_cursor);

  return possible;
}

static const MonitorHost *find_replica_round_robin_locality_aware(
  const condition_handler handler, const void *ctx
) {
  const MonitorHost *alive[LOCALITY_COUNT] = {0};
  const MonitorHost *possible[LOCALITY_COUNT] = {0};
  const unsigned int best_rank = parameters.dc_locality_enabled ? LOCALITY_DC
                                                                : LOCALITY_GEO;

  unsigned int cursor = next_replica_round_robin();
  const unsigned int start_cursor = cursor;

  do {
    const MonitorHost *mon_host = &monitor_host_list[cursor];
    const MonitorSnapshot snap = atomic_get_snapshot(mon_host);

    if (handler(snap, mon_host, ctx)) {
      const unsigned int rank = locality_rank(mon_host);
      if (!snap.status.possible_dead && rank == best_rank) {
        return mon_host;
      }

      const MonitorHost **candidates = snap.status.possible_dead ? possible
                                                                 : alive;
      if (!candidates[rank]) {
        candidates[rank] = mon_host;
      }
    }

    cursor = next_cursor_in_circle(cursor);
  } while (cursor != start_cursor);

  for (unsigned int rank = 0; rank < LOCALITY_COUNT; rank++) {
    if (alive[rank]) {
      return alive[rank];
    }
  }
  for (unsigned int rank = 0; rank < LOCALITY_COUNT; rank++) {
    if (possible[rank]) {
      return possible[rank];
    }
  }
  return nullptr;
}

const MonitorHost *find_replica(
  const condition_handler handler, const void *ctx, const char *log_context
) {
  if (host_count == 0) {
    return nullptr;
  }

  const MonitorHost *replica = nullptr;

  if (parameters.dc_locality_enabled || parameters.geo_locality_enabled) {
    replica = find_replica_round_robin_locality_aware(handler, ctx);
  } else {
    replica = find_replica_round_robin_plain(handler, ctx);
  }

  if (replica) {
    return replica;
  }

  const MonitorHost *master = get_master_monitor_host();
  if (master) {
    pg_status_log(
      PG_STATUS_LOG_DEBUG, "selection",
      "master returned instead of replica context=%s", log_context
    );
  }
  return master;
}

const MonitorHost *find_most_sync_replica_by_bytes(
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
    return best_alive;
  }
  if (best_possible) {
    return best_possible;
  }

  const MonitorHost *master = get_master_monitor_host();
  if (master) {
    pg_status_log(
      PG_STATUS_LOG_DEBUG, "selection",
      "master returned instead of replica context=%s", log_context
    );
  }
  return master;
}
