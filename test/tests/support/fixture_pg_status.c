/**
 * Support fixture and assertions specific to pg-status API tests.
 */

#include "fixture_pg_status.h"

#include <cjson/cJSON.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "http_test.h"

static constexpr size_t FORMATTED_LSN_CAPACITY = sizeof("FFFFFFFF/FFFFFFFF");

TestHost fixture_pg_status_master_host(char *name) {
  return (TestHost){
    .host = name,
    .snapshot = {
      .status = {.master = true, .alive = true, .possible_dead = false},
      .lag_ms = 0,
      .lag_bytes = 0,
      .lsn = 0x500,
    },
  };
}

TestHost fixture_pg_status_replica_host(
  char *name, const uint64_t lag_ms, const uint64_t lag_bytes,
  const uint64_t lsn
) {
  return (TestHost){
    .host = name,
    .snapshot = {
      .status = {.master = false, .alive = true, .possible_dead = false},
      .lag_ms = lag_ms,
      .lag_bytes = lag_bytes,
      .lsn = lsn,
    },
  };
}

TestHost fixture_pg_status_possible_dead_replica_host(
  char *name, const uint64_t lag_ms, const uint64_t lag_bytes,
  const uint64_t lsn
) {
  TestHost host = fixture_pg_status_replica_host(name, lag_ms, lag_bytes, lsn);
  host.snapshot.status.possible_dead = true;
  return host;
}

TestHost fixture_pg_status_dead_host(char *name) {
  return (TestHost){
    .host = name,
    .snapshot = {
      .status = {.master = false, .alive = false, .possible_dead = false},
      .lag_ms = 0,
      .lag_bytes = 0,
      .lsn = 0,
    },
  };
}

char *fixture_pg_status_format_expected_lsn(const uint64_t lsn) {
  char *formatted_lsn = malloc(FORMATTED_LSN_CAPACITY);
  http_test_assert_true(formatted_lsn != nullptr, "expected LSN allocation");

  const int length = snprintf(
    formatted_lsn, FORMATTED_LSN_CAPACITY, "%" PRIX32 "/%" PRIX32,
    (uint32_t)(lsn >> 32), (uint32_t)lsn
  );
  http_test_assert_true(
    length > 0 && (size_t)length < FORMATTED_LSN_CAPACITY, "expected LSN format"
  );
  return formatted_lsn;
}

PgStatusApiFixture fixture_pg_status_start(
  const TestHost *hosts, const size_t count, const int master_index
) {
  http_test_assert_true(
    count > 0 && count <= MAX_HOSTS, "invalid test host count"
  );
  host_count = (unsigned int)count;
  for (size_t i = 0; i < count; i++) {
    monitor_host_list[i].host = hosts[i].host;
    publish_monitor_snapshot(&monitor_host_list[i], hosts[i].snapshot);
  }
  save_master_index(master_index);

  HTTPServer *server = start_pg_status_api("127.0.0.1", 0);
  return (PgStatusApiFixture){
    .server = server,
    .port = http_server_port(server),
  };
}

void fixture_pg_status_stop(PgStatusApiFixture *api) {
  stop_http_server(api->server);
  *api = (PgStatusApiFixture){0};
}

void fixture_pg_status_assert_json_body(
  const TestHTTPResponse *response, const char *expected_json
) {
  cJSON *actual = cJSON_Parse(response->body);
  cJSON *expected = cJSON_Parse(expected_json);
  if (!actual || !expected || !cJSON_Compare(actual, expected, true)) {
    fprintf(
      stderr, "Expected JSON: %s\nActual body: %s\n", expected_json,
      response->body
    );
    cJSON_Delete(actual);
    cJSON_Delete(expected);
    http_test_fail("unexpected JSON body");
  }
  cJSON_Delete(actual);
  cJSON_Delete(expected);
}

void fixture_pg_status_expect_text(
  const TestHTTPResponse *response, const unsigned int status, const char *body
) {
  http_test_assert_status(response, status);
  http_test_assert_body(response, body);
}

void fixture_pg_status_expect_json(
  const TestHTTPResponse *response, const unsigned int status,
  const char *expected_json
) {
  http_test_assert_status(response, status);
  http_test_assert_contains(response, "Content-Type: application/json");
  fixture_pg_status_assert_json_body(response, expected_json);
}
