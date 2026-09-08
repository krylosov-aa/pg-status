/**
 * Utilities for initializing and writing an array of monitoring hosts
 */

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "logger.h"
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
static atomic_int master_index = -1;

/**
 * Atomically gets the current master position in the array
 */
int get_master_index(void) {
  return atomic_load_explicit(&master_index, memory_order_relaxed);
}

/**
 * Initializes MonitorHost to its initial value.
 */
static void init_monitor_host(
  MonitorHost *monitor_host, char *host, char *port
) {
  monitor_host->host = copy_string(host);
  monitor_host->port = copy_string(port);
  monitor_host->dc = nullptr;
  monitor_host->geo = nullptr;
  monitor_host->failed_connections = 0;

  monitor_host->conn = nullptr;
  monitor_host->poll_state = HOST_POLL_IDLE;
  monitor_host->poll_events = 0;
  monitor_host->next_poll_at_ms = 0;
  monitor_host->iter_deadline_ms = 0;
  monitor_host->connected_at_ms = 0;
  monitor_host->pollfd_slot = -1;
  monitor_host->wal_receiver_disabled = false;
  monitor_host->iter_retry_without_wal_receiver = false;
  monitor_host->iter_data_ready = false;
  monitor_host->iter_new_status = (MonitorStatus){
    .alive = false, .master = false, .possible_dead = false
  };
  monitor_host->iter_new_lag_ms = 0;
  monitor_host->iter_new_lag_bytes = 0;
  monitor_host->iter_new_lsn = 0;

  atomic_store_explicit(&monitor_host->seq, 0, memory_order_relaxed);
  atomic_store_explicit(
    &monitor_host->status,
    ((MonitorStatus){.alive = false, .master = false, .possible_dead = true}),
    memory_order_relaxed
  );
  atomic_store_explicit(&monitor_host->lag_ms, 0, memory_order_relaxed);
  atomic_store_explicit(&monitor_host->lag_bytes, 0, memory_order_relaxed);
  atomic_store_explicit(&monitor_host->lsn, 0, memory_order_relaxed);
}

static bool locality_values_are_complete(const char *values) {
  if (!values) {
    return false;
  }

  unsigned int count = 0;
  const char *value = values;
  for (;;) {
    const char *separator = strchr(value, ',');
    const char *end = separator ? separator : value + strlen(value);
    if (end == value) {
      return false;
    }
    count++;
    if (!separator) {
      break;
    }
    value = separator + 1;
  }
  return count == host_count;
}

static void assign_locality_values(const char *values, const bool is_dc) {
  char *items = copy_string(values);
  char *item = items;
  for (unsigned int i = 0; i < host_count; i++) {
    char *separator = strchr(item, ',');
    if (separator) {
      *separator = '\0';
    }
    if (is_dc) {
      monitor_host_list[i].dc = copy_string(item);
    } else {
      monitor_host_list[i].geo = copy_string(item);
    }
    item = separator ? separator + 1 : nullptr;
  }
  free(items);
}

static bool init_locality(
  const char *label, const char *hosts_parameter_name,
  const char *current_parameter_name, const char *current_env_parameter_name,
  const char *values, const char *current, const bool configured,
  const bool is_dc
) {
  const bool values_complete = locality_values_are_complete(values);
  if (values_complete) {
    assign_locality_values(values, is_dc);
  }

  const bool enabled = values_complete && current && *current;
  if (configured && !enabled) {
    pg_status_log(
      PG_STATUS_LOG_WARNING, "config",
      "%s preference disabled: incomplete locality configuration; set %s "
      "and %s or %s",
      label, hosts_parameter_name, current_parameter_name,
      current_env_parameter_name
    );
  }
  return enabled;
}

/**
 * Initializes the MonitorHost array to its initial values.
 */
void init_monitor_host_list(void) {
  char *hosts = copy_string(parameters.hosts);
  char *host_save_ptr = nullptr;
  char *host = strtok_r(hosts, ",", &host_save_ptr);

  char *ports = copy_string(parameters.port);
  char *port_save_ptr = nullptr;
  char *port = strtok_r(ports, ",", &port_save_ptr);
  if (!port) {
    free(hosts);
    free(ports);
    pg_status_log_fatal(
      "config", "pg_status__pg_port must contain at least one port"
    );
  }

  while (host) {
    if (host_count == MAX_HOSTS) {
      pg_status_log_fatal("config", "too many hosts; maximum=%d", MAX_HOSTS);
    }
    init_monitor_host(&monitor_host_list[host_count], host, port);

    host = strtok_r(nullptr, ",", &host_save_ptr);
    char *next_port = strtok_r(nullptr, ",", &port_save_ptr);
    if (next_port) {
      port = next_port;
    }
    host_count++;
  }
  if (host_count == 0) {
    pg_status_log_fatal("config", "host count must be greater than 0");
  }

  parameters.dc_locality_enabled = init_locality(
    "DC", "pg_status__hosts_dc", "pg_status__current_dc",
    "pg_status__current_dc_env", parameters.hosts_dc, parameters.current_dc,
    parameters.dc_locality_configured, true
  );
  parameters.geo_locality_enabled = init_locality(
    "geo", "pg_status__hosts_geo", "pg_status__current_geo",
    "pg_status__current_geo_env", parameters.hosts_geo, parameters.current_geo,
    parameters.geo_locality_configured, false
  );
  free(hosts);
  free(ports);
}

/**
 * Atomically saves the current master's host
 */
void save_master_index(const int i) {
  atomic_store_explicit(&master_index, i, memory_order_relaxed);
}

void publish_monitor_snapshot(
  MonitorHost *host, const MonitorSnapshot snapshot
) {
  atomic_fetch_add_explicit(&host->seq, 1, memory_order_relaxed);

  // Release stores keep the odd sequence marker before the snapshot data.
  atomic_store_explicit(&host->lag_ms, snapshot.lag_ms, memory_order_release);
  atomic_store_explicit(
    &host->lag_bytes, snapshot.lag_bytes, memory_order_release
  );
  atomic_store_explicit(&host->lsn, snapshot.lsn, memory_order_release);
  atomic_store_explicit(&host->status, snapshot.status, memory_order_release);
  // Publish the complete snapshot by restoring an even sequence number.
  atomic_fetch_add_explicit(&host->seq, 1, memory_order_release);
}
