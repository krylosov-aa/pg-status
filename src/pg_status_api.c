/**
 * Public HTTP API of pg-status.
 */

#include "pg_status_api.h"

#include <cjson/cJSON.h>
#include <stdint.h>
#include <stdlib.h>

#include "pg_monitor.h"
#include "pg_status_version.h"
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
  add_bool_to_json_object(json_obj, "possible_dead", snap.status.possible_dead);
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
  (void)request;
  cJSON *arr = json_array();
  for (unsigned int i = 0; i < host_count; i++) {
    const MonitorHost *mon_host = &monitor_host_list[i];
    cJSON *json_obj = json_object();
    add_host_to_json(json_obj, mon_host->host);

    const MonitorSnapshot snap = atomic_get_snapshot(mon_host);
    add_host_status_to_json(json_obj, snap);
    cJSON_AddItemToArray(arr, json_obj);
  }

  http_response_set_owned_body(
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
    http_response_set_owned_body(
      response, json_to_str(json_obj), "application/json", cJSON_free
    );
  } else {
    http_response_set_borrowed_body(
      response, host, host ? "text/plain; charset=utf-8" : nullptr
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
  if (!parse_get_param_uint(request, "lag_ms", &thresholds->max_lag_ms)) {
    bad_request(response, "{\"error_text\": \"Invalid lag_ms\"}");
    return false;
  }
  if (!parse_get_param_uint(request, "lag_bytes", &thresholds->max_lag_bytes)) {
    bad_request(response, "{\"error_text\": \"Invalid lag_bytes\"}");
    return false;
  }
  if (!parse_get_param_lsn(request, "min_lsn", &thresholds->min_lsn)) {
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
  if (!parse_lag_thresholds(&thresholds, request, response)) {
    return;
  }

  const char *host = find_replica_round_robin(
    is_sync_replica_by_time_and_bytes, &thresholds, "/replica"
  );
  return_single_host(request, response, host);
}

static void get_master(const HTTPRequest *request, HTTPResponse *response) {
  return_single_host(request, response, get_master_host());
}

static void get_sync_host_by_time(
  const HTTPRequest *request, HTTPResponse *response
) {
  LagThresholds thresholds = lag_thresholds_by_parameters();
  if (!parse_lag_thresholds(&thresholds, request, response)) {
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
  if (!parse_lag_thresholds(&thresholds, request, response)) {
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
  if (!parse_lag_thresholds(&thresholds, request, response)) {
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
  if (!parse_lag_thresholds(&thresholds, request, response)) {
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
  if (!parse_lag_thresholds(&thresholds, request, response)) {
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
  http_response_set_owned_body(
    response, json_to_str(json_obj), "application/json", cJSON_free
  );
}

static void get_version(const HTTPRequest *request, HTTPResponse *response) {
  (void)request;
  http_response_set_borrowed_body(
    response, PG_STATUS_VERSION, "text/plain; charset=utf-8"
  );
}

static const Route routes[] = {
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

HTTPServer *start_pg_status_api(
  const char *listen_address, const uint16_t port
) {
  return start_http_server(
    listen_address, port, routes, sizeof(routes) / sizeof(routes[0])
  );
}
