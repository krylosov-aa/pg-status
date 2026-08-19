#include <cjson/cJSON.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "http_server.h"
#include "pg_monitor.h"
#include "utils.h"

static void add_host_to_json(cJSON *json_obj, const char *host) {
  if (!host) {
    add_null_to_json_object(json_obj, "host");
  } else {
    add_str_to_json_object(json_obj, "host", host);
  }
}

static void add_host_status_to_json(
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
      json_obj, "sync_by_bytes", snap.lag_bytes <= parameters.sync_max_lag_bytes
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

static void get_all_hosts(const HTTPRequest *request, HTTPResponse *response) {
  cJSON *arr = json_array();
  for (unsigned int i = 0; i < host_count; i++) {
    const MonitorHost *mon_host = &monitor_host_list[i];
    cJSON *json_obj = json_object();
    add_host_to_json(json_obj, mon_host->host);

    const MonitorSnapshot snap = atomic_get_snapshot(mon_host);
    add_host_status_to_json(json_obj, snap);

    cJSON_AddItemToArray(arr, json_obj);
  }

  http_response_set_body(
    response, json_to_str(arr), "application/json", cJSON_free
  );
}

static void return_single_host(
  const HTTPRequest *request, HTTPResponse *response, const char *host
) {
  if (!host) {
    http_response_set_status(response, 404);
  }

  if (http_request_accepts_json(request)) {
    cJSON *json_obj = json_object();
    add_host_to_json(json_obj, host);
    http_response_set_body(
      response, json_to_str(json_obj), "application/json", cJSON_free
    );
  } else {
    http_response_set_body(
      response, host, host ? "text/plain; charset=utf-8" : nullptr, nullptr
    );
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
  LagThresholds *thresholds, const HTTPRequest *request, HTTPResponse *response
) {
  bool parsed = parse_get_param_uint(
    request, "lag_ms", &thresholds->max_lag_ms
  );
  if (!parsed) {
    bad_request(response, "{\"error_text\": \"Invalid lag_ms\"}");
    return false;
  }

  parsed = parse_get_param_uint(
    request, "lag_bytes", &thresholds->max_lag_bytes
  );
  if (!parsed) {
    bad_request(response, "{\"error_text\": \"Invalid lag_bytes\"}");
    return false;
  }

  parsed = parse_get_param_lsn(request, "min_lsn", &thresholds->min_lsn);
  if (!parsed) {
    bad_request(response, "{\"error_text\": \"Invalid min_lsn\"}");
    return false;
  }

  return true;
}

static void get_random_replica(
  const HTTPRequest *request, HTTPResponse *response
) {
  LagThresholds thresholds = {
    .max_lag_ms = UINT64_MAX,
    .max_lag_bytes = UINT64_MAX,
    .min_lsn = 0,
  };
  const bool parsed = parse_lag_thresholds(&thresholds, request, response);
  if (!parsed) {
    return;
  }

  const char *host = find_replica_round_robin(
    is_sync_replica_by_time_and_bytes, &thresholds, "/replica"
  );
  return_single_host(request, response, host);
}

static void get_master(const HTTPRequest *request, HTTPResponse *response) {
  const char *host = get_master_host();
  return_single_host(request, response, host);
}

static void get_sync_host_by_time(
  const HTTPRequest *request, HTTPResponse *response
) {
  LagThresholds thresholds = lag_thresholds_by_parameters();
  const bool parsed = parse_lag_thresholds(&thresholds, request, response);
  if (!parsed) {
    return;
  }

  const char *host = find_replica_round_robin(
    is_sync_replica_by_time, &thresholds, "/sync_by_time"
  );
  return_single_host(request, response, host);
}

static void get_sync_host_by_bytes(
  const HTTPRequest *request, HTTPResponse *response
) {
  LagThresholds thresholds = lag_thresholds_by_parameters();
  const bool parsed = parse_lag_thresholds(&thresholds, request, response);
  if (!parsed) {
    return;
  }

  const char *host = find_replica_round_robin(
    is_sync_replica_by_bytes, &thresholds, "/sync_by_bytes"
  );
  return_single_host(request, response, host);
}

static void get_sync_host_by_time_or_bytes(
  const HTTPRequest *request, HTTPResponse *response
) {
  LagThresholds thresholds = lag_thresholds_by_parameters();
  const bool parsed = parse_lag_thresholds(&thresholds, request, response);
  if (!parsed) {
    return;
  }

  const char *host = find_replica_round_robin(
    is_sync_replica_by_time_or_bytes, &thresholds, "/sync_by_time_or_bytes"
  );
  return_single_host(request, response, host);
}

static void get_sync_host_by_time_and_bytes(
  const HTTPRequest *request, HTTPResponse *response
) {
  LagThresholds thresholds = lag_thresholds_by_parameters();
  const bool parsed = parse_lag_thresholds(&thresholds, request, response);
  if (!parsed) {
    return;
  }

  const char *host = find_replica_round_robin(
    is_sync_replica_by_time_and_bytes, &thresholds, "/sync_by_time_and_bytes"
  );
  return_single_host(request, response, host);
}

static void get_most_sync_host_by_bytes(
  const HTTPRequest *request, HTTPResponse *response
) {
  LagThresholds thresholds = lag_thresholds_by_parameters();
  const bool parsed = parse_lag_thresholds(&thresholds, request, response);
  if (!parsed) {
    return;
  }

  const char *host = find_most_sync_replica_by_bytes(
    &thresholds, "/most_sync_by_bytes"
  );
  return_single_host(request, response, host);
}

static void get_host_status(
  const HTTPRequest *request, HTTPResponse *response
) {
  const char *host = http_request_get_query_param(request, "host");
  if (!host) {
    bad_request(
      response, "{\"error_text\": \"Get parameter 'host' wasn't passed\"}"
    );
    return;
  }
  const MonitorHost *mon_host = find_host_by_name(host);
  if (!mon_host) {
    http_response_set_status(response, 404);
    return;
  }

  const MonitorSnapshot snap = atomic_get_snapshot(mon_host);
  cJSON *json_obj = json_object();
  add_host_status_to_json(json_obj, snap);

  http_response_set_body(
    response, json_to_str(json_obj), "application/json", cJSON_free
  );
}

static void block_termination_signals(sigset_t *sigset) {
  sigemptyset(sigset);
  sigaddset(sigset, SIGINT);
  sigaddset(sigset, SIGTERM);

  if (pthread_sigmask(SIG_BLOCK, sigset, NULL) != 0) {
    raise_error("pthread_sigmask");
  }
}

static void get_version(const HTTPRequest *request, HTTPResponse *response) {
  http_response_set_body(
    response, "2.1.1", "text/plain; charset=utf-8", nullptr
  );
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

static const char *get_http_listen_address(void) {
  const char *env_val = getenv("pg_status__http_listen_address");
  if (env_val && *env_val) {
    return env_val;
  }
  return "0.0.0.0";
}

static Route routes[] = {
  {.path = "/master", .handler = get_master},
  {.path = "/replica", .handler = get_random_replica},
  {.path = "/hosts", .handler = get_all_hosts},
  {.path = "/status", .handler = get_host_status},
  {.path = "/sync_by_time", .handler = get_sync_host_by_time},
  {.path = "/sync_by_bytes", .handler = get_sync_host_by_bytes},
  {.path = "/sync_by_time_or_bytes", .handler = get_sync_host_by_time_or_bytes},
  {.path = "/sync_by_time_and_bytes",
   .handler = get_sync_host_by_time_and_bytes},
  {.path = "/most_sync_by_bytes", .handler = get_most_sync_host_by_bytes},
  {.path = "/version", .handler = get_version},
};

int main() {
  sigset_t sigset;
  block_termination_signals(&sigset);

  start_pg_monitor();
  HTTPServer *server = start_http_server(
    get_http_listen_address(), get_port(), routes,
    sizeof(routes) / sizeof(routes[0])
  );

  wait_for_termination_signal(&sigset);

  stop_http_server(server);
  stop_pg_monitor();
  return 0;
}
