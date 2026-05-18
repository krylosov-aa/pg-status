#include <cjson/cJSON.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "http_server.h"
#include "pg_monitor.h"
#include "utils.h"

void get_all_hosts(MHD_Connection *connection, HTTPResponse *response) {
  size_t len = 0;
  char *json = copy_hosts_cache(&len);
  if (!json) {
    response->const_response = "[]";
    response->content_type = "application/json";
    return;
  }
  response->response = json;
  response->memory_mode = MHD_RESPMEM_MUST_FREE;
  response->content_type = "application/json";
}

static void return_single_host(HTTPResponse *response, const char *host) {
  if (!host) {
    response->status_code = 404;
  }

  if (need_json_response(response)) {
    cJSON *json_obj = json_object();
    add_host_to_json(json_obj, host);
    response->response = json_to_str(json_obj);
    response->memory_mode = MHD_RESPMEM_MUST_FREE;
  } else {
    response->const_response = host;
  }
}

static LagThresholds lag_thresholds_by_parameters(void) {
  return (LagThresholds){
    .max_lag_ms = parameters.sync_max_lag_ms,
    .max_lag_bytes = parameters.sync_max_lag_bytes,
    .min_lsn = 0,
  };
}

static bool parse_lag_thresholds(
  LagThresholds *thresholds, MHD_Connection *connection, HTTPResponse *response
) {
  bool parsed = parse_get_param_uint(
    connection, "lag_ms", &thresholds->max_lag_ms
  );
  if (!parsed) {
    bad_request(response, "{\"error_text\": \"Invalid lag_ms\"}");
    return false;
  }

  parsed = parse_get_param_uint(
    connection, "lag_bytes", &thresholds->max_lag_bytes
  );
  if (!parsed) {
    bad_request(response, "{\"error_text\": \"Invalid lag_bytes\"}");
    return false;
  }

  parsed = parse_get_param_lsn(connection, "min_lsn", &thresholds->min_lsn);
  if (!parsed) {
    bad_request(response, "{\"error_text\": \"Invalid min_lsn\"}");
    return false;
  }

  return true;
}

static void get_random_replica(
  MHD_Connection *connection, HTTPResponse *response
) {
  LagThresholds thresholds = {
    .max_lag_ms = UINT64_MAX,
    .max_lag_bytes = UINT64_MAX,
    .min_lsn = 0,
  };
  const bool parsed = parse_lag_thresholds(&thresholds, connection, response);
  if (!parsed) {
    return;
  }

  const char *host = find_replica_round_robin(
    is_sync_replica_by_time_and_bytes, &thresholds, "/replica"
  );
  return_single_host(response, host);
}

static void get_master(MHD_Connection *connection, HTTPResponse *response) {
  const char *host = get_master_host();
  return_single_host(response, host);
}

static void get_sync_host_by_time(
  MHD_Connection *connection, HTTPResponse *response
) {
  LagThresholds thresholds = lag_thresholds_by_parameters();
  const bool parsed = parse_lag_thresholds(&thresholds, connection, response);
  if (!parsed) {
    return;
  }

  const char *host = find_replica_round_robin(
    is_sync_replica_by_time, &thresholds, "/sync_by_time"
  );
  return_single_host(response, host);
}

static void get_sync_host_by_bytes(
  MHD_Connection *connection, HTTPResponse *response
) {
  LagThresholds thresholds = lag_thresholds_by_parameters();
  const bool parsed = parse_lag_thresholds(&thresholds, connection, response);
  if (!parsed) {
    return;
  }

  const char *host = find_replica_round_robin(
    is_sync_replica_by_bytes, &thresholds, "/sync_by_bytes"
  );
  return_single_host(response, host);
}

static void get_sync_host_by_time_or_bytes(
  MHD_Connection *connection, HTTPResponse *response
) {
  LagThresholds thresholds = lag_thresholds_by_parameters();
  const bool parsed = parse_lag_thresholds(&thresholds, connection, response);
  if (!parsed) {
    return;
  }

  const char *host = find_replica_round_robin(
    is_sync_replica_by_time_or_bytes, &thresholds, "/sync_by_time_or_bytes"
  );
  return_single_host(response, host);
}

static void get_sync_host_by_time_and_bytes(
  MHD_Connection *connection, HTTPResponse *response
) {
  LagThresholds thresholds = lag_thresholds_by_parameters();
  const bool parsed = parse_lag_thresholds(&thresholds, connection, response);
  if (!parsed) {
    return;
  }

  const char *host = find_replica_round_robin(
    is_sync_replica_by_time_and_bytes, &thresholds, "/sync_by_time_and_bytes"
  );
  return_single_host(response, host);
}

static void get_most_sync_host_by_bytes(
  MHD_Connection *connection, HTTPResponse *response
) {
  LagThresholds thresholds = lag_thresholds_by_parameters();
  const bool parsed = parse_lag_thresholds(&thresholds, connection, response);
  if (!parsed) {
    return;
  }

  const char *host = find_most_sync_replica_by_bytes(
    &thresholds, "/most_sync_by_bytes"
  );
  return_single_host(response, host);
}

static void get_host_status(
  MHD_Connection *connection, HTTPResponse *response
) {
  const char *host = MHD_lookup_connection_value(
    connection, MHD_GET_ARGUMENT_KIND, "host"
  );
  if (!host) {
    bad_request(
      response, "{\"error_text\": \"Get parameter 'host' wasn't passed\"}"
    );
    return;
  }
  const MonitorHost *mon_host = find_host_by_name(host);
  if (!mon_host) {
    response->status_code = 404;
    return;
  }

  const MonitorSnapshot snap = atomic_get_snapshot(mon_host);
  cJSON *json_obj = json_object();
  add_host_status_to_json(json_obj, snap);

  response->response = json_to_str(json_obj);
  response->memory_mode = MHD_RESPMEM_MUST_FREE;
  response->content_type = "application/json";
}

static void block_termination_signals(sigset_t *sigset) {
  sigemptyset(sigset);
  sigaddset(sigset, SIGINT);
  sigaddset(sigset, SIGTERM);

  if (pthread_sigmask(SIG_BLOCK, sigset, NULL) != 0) {
    raise_error("pthread_sigmask");
  }
}

static void get_version(MHD_Connection *connection, HTTPResponse *response) {
  response->const_response = "2.0.0";
}

static void wait_for_termination_signal(const sigset_t *sigset) {
  int sig;

  if (sigwait(sigset, &sig) == 0) {
    switch (sig) {
      case SIGINT:
        printf("Received SIGINT\n");
        break;
      case SIGTERM:
        printf("Received SIGTERM\n");
        break;
      default:
        printf("Received signal %d\n", sig);
    }
  }
}

static uint16_t get_port() {
  const char *env_val = getenv("pg_status__http_port");
  if (env_val && *env_val) {
    return str_to_uint16(env_val);
  }
  return 8000;
}

static Route routes[] = {
  {"GET", "/master", get_master},
  {"GET", "/replica", get_random_replica},
  {"GET", "/hosts", get_all_hosts},
  {"GET", "/status", get_host_status},
  {"GET", "/sync_by_time", get_sync_host_by_time},
  {"GET", "/sync_by_bytes", get_sync_host_by_bytes},
  {"GET", "/sync_by_time_or_bytes", get_sync_host_by_time_or_bytes},
  {"GET", "/sync_by_time_and_bytes", get_sync_host_by_time_and_bytes},
  {"GET", "/most_sync_by_bytes", get_most_sync_host_by_bytes},
  {"GET", "/version", get_version},
};

int main() {
  sigset_t sigset;
  block_termination_signals(&sigset);

  start_pg_monitor();
  MHD_Daemon *daemon = start_http_server(
    get_port(), routes, sizeof(routes) / sizeof(routes[0])
  );

  wait_for_termination_signal(&sigset);

  stop_pg_monitor();
  stop_http_server(daemon);
  return 0;
}
