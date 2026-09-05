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

  monitor_host->conn = nullptr;
  monitor_host->poll_state = HOST_POLL_IDLE;
  monitor_host->poll_events = 0;
  monitor_host->next_poll_at_ms = 0;
  monitor_host->iter_deadline_ms = 0;
  monitor_host->connected_at_ms = 0;
  monitor_host->pollfd_slot = -1;
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
