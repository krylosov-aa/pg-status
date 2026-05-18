/**
 * Utilities for initializing and writing an array of monitoring hosts
 */

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "pg_monitor.h"
#include "utils.h"

/**
 * The actual number of hosts
 */
unsigned int host_count = 0;

/**
 * Array of monitoring hosts
 */
MonitorHost monitor_host_list[MAX_HOSTS] = {0};

/**
 * Just a master host index in the array to find it asap
 */
atomic_int master_index = -1;

/**
 * Atomically gets the current master position in the array
 */
int get_master_index(void) {
  return atomic_load_explicit(&master_index, memory_order_relaxed);
}

/**
 * Returns hosts sequentially. When no more hosts are available,
 * it returns nullptr.
 */
static char *next_host(char *hosts) {
  static char *save_ptr = nullptr;
  static char *host = nullptr;

  if (host == nullptr) {
    host = strtok_r(hosts, ",", &save_ptr);
  } else {
    host = strtok_r(nullptr, ",", &save_ptr);
  }
  return host;
}

/**
 * Returns ports sequentially. When no more ports are available,
 * it continues returning the last one.
 * This allows a single port to be used for all hosts.
 */
static char *next_port(char *ports) {
  static char *last_port = nullptr;
  static char *save_ptr = nullptr;
  static char *port = nullptr;

  if (port == nullptr) {
    port = strtok_r(ports, ",", &save_ptr);
  } else {
    port = strtok_r(nullptr, ",", &save_ptr);
  }

  if (port != nullptr) {
    last_port = port;
  }
  return last_port;
}

/**
 * Returns the string to connect to pg
 */
static char *get_connection_string(char *host, char *port) {
  return format_string(
    "user=%s password=%s host=%s port=%s "
    "dbname=%s connect_timeout=%s",
    parameters.user, parameters.password, host, port, parameters.database,
    parameters.connect_timeout
  );
}

/**
 * Initializes MonitorHost to its initial value.
 */
static void init_monitor_host(
  MonitorHost *monitor_host, char *host, char *port
) {
  monitor_host->host = copy_string(host);
  monitor_host->connection_str = get_connection_string(host, port);
  monitor_host->failed_connections = 0;

  atomic_store_explicit(&monitor_host->seq, 0, memory_order_relaxed);
  atomic_store_explicit(
    &monitor_host->status,
    (MonitorStatus){.alive = false, .master = false, .possible_dead = false},
    memory_order_relaxed
  );
  atomic_store_explicit(&monitor_host->lag_ms, 0, memory_order_relaxed);
  atomic_store_explicit(&monitor_host->lag_bytes, 0, memory_order_relaxed);
  atomic_store_explicit(&monitor_host->lsn, 0, memory_order_relaxed);
}

/**
 * Initializes the MonitorHost array to its initial values.
 */
void init_monitor_host_list(void) {
  char *hosts = copy_string(parameters.hosts);
  char *host = next_host(hosts);

  char *ports = copy_string(parameters.port);
  char *port = next_port(ports);

  while (host) {
    if (host_count == MAX_HOSTS) {
      raise_error("Too many hosts. Maximum value = %d", MAX_HOSTS);
    }
    init_monitor_host(&monitor_host_list[host_count], host, port);

    host = next_host(hosts);
    port = next_port(ports);
    host_count++;
  }
  if (host_count == 0) {
    raise_error("host count must be greater then 0");
  }
  free(hosts);
  free(ports);
}

/**
 * Atomically saves the current master's host
 */
void save_master_index(const int i) {
  atomic_store_explicit(&master_index, i, memory_order_relaxed);
}

// ------------------------ /hosts JSON cache ------------------------

void add_host_to_json(cJSON *json_obj, const char *host) {
  if (!host) {
    add_null_to_json_object(json_obj, "host");
  } else {
    add_str_to_json_object(json_obj, "host", host);
  }
}

void add_host_status_to_json(
  cJSON *json_obj, const MonitorSnapshot snap
) {
  add_bool_to_json_object(json_obj, "master", snap.status.master);
  add_bool_to_json_object(json_obj, "alive", snap.status.alive);
  if (snap.status.alive) {
    add_uint64_to_json_object(json_obj, "lag_ms", snap.lag_ms);
    add_bool_to_json_object(
      json_obj, "sync_by_time", snap.lag_ms <= parameters.sync_max_lag_ms
    );

    add_uint64_to_json_object(json_obj, "lag_bytes", snap.lag_bytes);
    add_bool_to_json_object(
      json_obj, "sync_by_bytes",
      snap.lag_bytes <= parameters.sync_max_lag_bytes
    );

    char *lsn_str = format_lsn(snap.lsn);
    add_str_to_json_object(json_obj, "lsn", lsn_str);
    free(lsn_str);
  } else {
    add_null_to_json_object(json_obj, "lag_ms");
    add_bool_to_json_object(json_obj, "sync_by_time", false);
    add_null_to_json_object(json_obj, "lag_bytes");
    add_bool_to_json_object(json_obj, "sync_by_bytes", false);
    add_null_to_json_object(json_obj, "lsn");
  }
}

/**
 * One generation of the /hosts JSON cache. Pointer to the current
 * generation lives in `hosts_cache` and is updated atomically.
 */
typedef struct {
  char *str;
  size_t len;
} HostsCacheEntry;

/**
 * Currently published /hosts JSON. Readers atomically load this and
 * memcpy out before returning to MHD.
 */
static _Atomic(HostsCacheEntry *) hosts_cache = nullptr;

/**
 * Previous generation kept alive for one extra cycle as a grace period
 * for any reader that loaded `hosts_cache` just before the writer swapped
 * it. Writer-private — only the poll thread touches this.
 */
static HostsCacheEntry *prev_hosts_cache = nullptr;

static void free_hosts_cache_entry(HostsCacheEntry *entry) {
  if (entry) {
    free(entry->str);
    free(entry);
  }
}

/**
 * Builds a fresh /hosts JSON string. Runs outside any critical section
 * so the writer doesn't compete with readers during cJSON allocations.
 */
static char *build_hosts_json(size_t *len) {
  cJSON *arr = json_array();
  for (unsigned int i = 0; i < host_count; i++) {
    const MonitorHost *mon_host = &monitor_host_list[i];
    cJSON *json_obj = json_object();
    add_host_to_json(json_obj, mon_host->host);

    const MonitorSnapshot snap = atomic_get_snapshot(mon_host);
    add_host_status_to_json(json_obj, snap);

    cJSON_AddItemToArray(arr, json_obj);
  }
  char *result = json_to_str(arr);
  *len = strlen(result);
  return result;
}

void update_hosts_cache(void) {
  HostsCacheEntry *next = malloc(sizeof(HostsCacheEntry));
  if (!next) {
    raise_error("Can't allocate /hosts cache entry");
  }
  next->str = build_hosts_json(&next->len);

  HostsCacheEntry *swapped = atomic_exchange_explicit(
    &hosts_cache, next, memory_order_acq_rel
  );

  // Free the generation older than `swapped` — by the time the writer
  // is on iteration N, no in-flight reader still holds a pointer from
  // iteration N-2 (HTTP requests finish in milliseconds; check_hosts
  // cycles are sub-second at minimum).
  free_hosts_cache_entry(prev_hosts_cache);
  prev_hosts_cache = swapped;
}

char *copy_hosts_cache(size_t *len) {
  const HostsCacheEntry *entry = atomic_load_explicit(
    &hosts_cache, memory_order_acquire
  );
  if (!entry) {
    *len = 0;
    return nullptr;
  }
  char *copy = malloc(entry->len + 1);
  if (!copy) {
    raise_error("Can't allocate /hosts cache copy");
  }
  memcpy(copy, entry->str, entry->len + 1);
  *len = entry->len;
  return copy;
}

void free_hosts_cache(void) {
  HostsCacheEntry *entry = atomic_exchange_explicit(
    &hosts_cache, nullptr, memory_order_acq_rel
  );
  free_hosts_cache_entry(entry);
  free_hosts_cache_entry(prev_hosts_cache);
  prev_hosts_cache = nullptr;
}
