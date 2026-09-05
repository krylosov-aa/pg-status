/**
 * Test scenarios for the public pg-status HTTP API.
 */

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fixture_pg_status.h"
#include "pg_status_version.h"
#include "utils.h"

static void test_version(void) {
  // Arrange
  parameters.sync_max_lag_ms = 100;
  parameters.sync_max_lag_bytes = 1000;
  const TestHost hosts[] = {
    fixture_pg_status_master_host("master"),
    fixture_pg_status_replica_host("replica", 50, 500, 0x450),
    fixture_pg_status_dead_host("dead"),
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );

  // Act
  TestHTTPResponse response = http_test_get(api.port, "/version", nullptr);

  // Assert
  http_test_assert_status(&response, 200);
  http_test_assert_contains(
    &response, "Content-Type: text/plain; charset=utf-8"
  );
  http_test_assert_body(&response, PG_STATUS_VERSION);

  // Cleanup
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_version_ignores_accept(void) {
  // Arrange
  const TestHost hosts[] = {fixture_pg_status_master_host("master")};
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );

  // Act
  TestHTTPResponse response = http_test_get(
    api.port, "/version", "Accept: application/json\r\n"
  );

  // Assert
  http_test_assert_status(&response, 200);
  http_test_assert_contains(
    &response, "Content-Type: text/plain; charset=utf-8"
  );
  http_test_assert_body(&response, PG_STATUS_VERSION);

  // Cleanup
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_master_text(void) {
  // Arrange
  parameters.sync_max_lag_ms = 100;
  parameters.sync_max_lag_bytes = 1000;
  const char *master_host_name = "master";
  const TestHost hosts[] = {
    fixture_pg_status_master_host(master_host_name),
    fixture_pg_status_replica_host("replica", 50, 500, 0x450),
    fixture_pg_status_dead_host("dead"),
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );

  // Act
  TestHTTPResponse response = http_test_get(api.port, "/master", nullptr);

  // Assert
  http_test_assert_status(&response, 200);
  http_test_assert_contains(
    &response, "Content-Type: text/plain; charset=utf-8"
  );
  http_test_assert_body(&response, master_host_name);

  // Cleanup
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_master_json(void) {
  // Arrange
  parameters.sync_max_lag_ms = 100;
  parameters.sync_max_lag_bytes = 1000;
  const char *master_host_name = "master";
  const TestHost hosts[] = {
    fixture_pg_status_master_host(master_host_name),
    fixture_pg_status_replica_host("replica", 50, 500, 0x450),
    fixture_pg_status_dead_host("dead"),
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );
  char *expected_json = format_string("{\"host\":\"%s\"}", master_host_name);

  // Act
  TestHTTPResponse response = http_test_get(
    api.port, "/master", "Accept: application/json\r\n"
  );

  // Assert
  fixture_pg_status_expect_json(&response, 200, expected_json);

  // Cleanup
  free(expected_json);
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_replica_json(void) {
  // Arrange
  const char *replica_host_name = "replica";
  const TestHost hosts[] = {
    fixture_pg_status_master_host("master"),
    fixture_pg_status_replica_host(replica_host_name, 50, 500, 0x450),
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );
  char *expected_json = format_string("{\"host\":\"%s\"}", replica_host_name);

  // Act
  TestHTTPResponse response = http_test_get(
    api.port, "/replica", "Accept: application/json\r\n"
  );

  // Assert
  fixture_pg_status_expect_json(&response, 200, expected_json);

  // Cleanup
  free(expected_json);
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_hosts(void) {
  // Arrange
  parameters.sync_max_lag_ms = 100;
  parameters.sync_max_lag_bytes = 1000;
  const char *master_host_name = "master";
  const char *replica_host_name = "replica";
  const char *dead_host_name = "dead";
  const TestHost master = fixture_pg_status_master_host(master_host_name);
  const TestHost replica = fixture_pg_status_replica_host(
    replica_host_name, 50, 500, 0x450
  );
  const TestHost dead = fixture_pg_status_dead_host(dead_host_name);
  const TestHost hosts[] = {
    master,
    replica,
    dead,
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );
  char *master_lsn = fixture_pg_status_format_expected_lsn(master.snapshot.lsn);
  char *replica_lsn = fixture_pg_status_format_expected_lsn(
    replica.snapshot.lsn
  );
  char *expected_json = format_string(
    "["
    "{\"host\":\"%s\",\"master\":true,\"possible_dead\":false,\"alive\":true,"
    "\"lag_ms\":%" PRIu64
    ",\"sync_by_time\":true,"
    "\"lag_bytes\":%" PRIu64
    ",\"sync_by_bytes\":true,\"lsn\":\"%s\"},"
    "{\"host\":\"%s\",\"master\":false,\"possible_dead\":false,\"alive\":true,"
    "\"lag_ms\":%" PRIu64
    ",\"sync_by_time\":true,"
    "\"lag_bytes\":%" PRIu64
    ",\"sync_by_bytes\":true,\"lsn\":\"%s\"},"
    "{\"host\":\"%s\",\"master\":false,\"possible_dead\":true,\"alive\":false,"
    "\"lag_ms\":null,\"sync_by_time\":false,\"lag_bytes\":null,"
    "\"sync_by_bytes\":false,\"lsn\":null}"
    "]",
    master.host, master.snapshot.lag_ms, master.snapshot.lag_bytes, master_lsn,
    replica.host, replica.snapshot.lag_ms, replica.snapshot.lag_bytes,
    replica_lsn, dead.host
  );

  // Act
  TestHTTPResponse response = http_test_get(api.port, "/hosts", nullptr);

  // Assert
  fixture_pg_status_expect_json(&response, 200, expected_json);

  // Cleanup
  free(expected_json);
  free(replica_lsn);
  free(master_lsn);
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_hosts_ignores_accept(void) {
  // Arrange
  parameters.sync_max_lag_ms = 100;
  parameters.sync_max_lag_bytes = 1000;
  const char *master_host_name = "master";
  const TestHost master = fixture_pg_status_master_host(master_host_name);
  const TestHost hosts[] = {master};
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );
  char *master_lsn = fixture_pg_status_format_expected_lsn(master.snapshot.lsn);
  char *expected_json = format_string(
    "[{\"host\":\"%s\",\"master\":true,\"possible_dead\":false,\"alive\":true,"
    "\"lag_ms\":%" PRIu64 ",\"sync_by_time\":true,\"lag_bytes\":%" PRIu64
    ",\"sync_by_bytes\":true,\"lsn\":\"%s\"}]",
    master.host, master.snapshot.lag_ms, master.snapshot.lag_bytes, master_lsn
  );

  // Act
  TestHTTPResponse response = http_test_get(
    api.port, "/hosts", "Accept: text/plain\r\n"
  );

  // Assert
  fixture_pg_status_expect_json(&response, 200, expected_json);

  // Cleanup
  free(expected_json);
  free(master_lsn);
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_status_alive(void) {
  // Arrange
  parameters.sync_max_lag_ms = 100;
  parameters.sync_max_lag_bytes = 1000;
  const char *replica_host_name = "replica";
  const TestHost replica = fixture_pg_status_replica_host(
    replica_host_name, 50, 500, 0x450
  );
  const TestHost hosts[] = {
    fixture_pg_status_master_host("master"),
    replica,
    fixture_pg_status_dead_host("dead"),
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );
  char *path = format_string("/status?host=%s", replica.host);
  char *lsn = fixture_pg_status_format_expected_lsn(replica.snapshot.lsn);
  char *expected_json = format_string(
    "{\"master\":false,\"possible_dead\":false,\"alive\":true,\"lag_ms\":"
    "%" PRIu64
    ","
    "\"sync_by_time\":true,\"lag_bytes\":%" PRIu64
    ","
    "\"sync_by_bytes\":true,\"lsn\":\"%s\"}",
    replica.snapshot.lag_ms, replica.snapshot.lag_bytes, lsn
  );

  // Act
  TestHTTPResponse response = http_test_get(api.port, path, nullptr);

  // Assert
  fixture_pg_status_expect_json(&response, 200, expected_json);

  // Cleanup
  free(expected_json);
  free(lsn);
  free(path);
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_status_mixed_sync(void) {
  // Arrange
  parameters.sync_max_lag_ms = 100;
  parameters.sync_max_lag_bytes = 1000;
  const char *replica_host_name = "replica";
  const TestHost replica = fixture_pg_status_replica_host(
    replica_host_name, parameters.sync_max_lag_ms + 1,
    parameters.sync_max_lag_bytes, 0x450
  );
  const TestHost hosts[] = {
    fixture_pg_status_master_host("master"),
    replica,
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );
  char *path = format_string("/status?host=%s", replica.host);
  char *lsn = fixture_pg_status_format_expected_lsn(replica.snapshot.lsn);
  char *expected_json = format_string(
    "{\"master\":false,\"possible_dead\":false,\"alive\":true,\"lag_ms\":"
    "%" PRIu64 ",\"sync_by_time\":false,\"lag_bytes\":%" PRIu64
    ",\"sync_by_bytes\":true,\"lsn\":\"%s\"}",
    replica.snapshot.lag_ms, replica.snapshot.lag_bytes, lsn
  );

  // Act
  TestHTTPResponse response = http_test_get(api.port, path, nullptr);

  // Assert
  fixture_pg_status_expect_json(&response, 200, expected_json);

  // Cleanup
  free(expected_json);
  free(lsn);
  free(path);
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_status_max_lsn(void) {
  // Arrange
  parameters.sync_max_lag_ms = 100;
  parameters.sync_max_lag_bytes = 1000;
  const char *replica_host_name = "replica";
  const TestHost replica = fixture_pg_status_replica_host(
    replica_host_name, 50, 500, UINT64_MAX
  );
  const TestHost hosts[] = {
    fixture_pg_status_master_host("master"),
    replica,
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );
  char *path = format_string("/status?host=%s", replica.host);
  char *lsn = fixture_pg_status_format_expected_lsn(replica.snapshot.lsn);
  char *expected_json = format_string(
    "{\"master\":false,\"possible_dead\":false,\"alive\":true,\"lag_ms\":"
    "%" PRIu64 ",\"sync_by_time\":true,\"lag_bytes\":%" PRIu64
    ",\"sync_by_bytes\":true,\"lsn\":\"%s\"}",
    replica.snapshot.lag_ms, replica.snapshot.lag_bytes, lsn
  );

  // Act
  TestHTTPResponse response = http_test_get(api.port, path, nullptr);

  // Assert
  fixture_pg_status_expect_json(&response, 200, expected_json);

  // Cleanup
  free(expected_json);
  free(lsn);
  free(path);
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_status_ignores_accept(void) {
  // Arrange
  parameters.sync_max_lag_ms = 100;
  parameters.sync_max_lag_bytes = 1000;
  const char *replica_host_name = "replica";
  const TestHost replica = fixture_pg_status_replica_host(
    replica_host_name, 50, 500, 0x450
  );
  const TestHost hosts[] = {
    fixture_pg_status_master_host("master"),
    replica,
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );
  char *path = format_string("/status?host=%s", replica.host);
  char *lsn = fixture_pg_status_format_expected_lsn(replica.snapshot.lsn);
  char *expected_json = format_string(
    "{\"master\":false,\"possible_dead\":false,\"alive\":true,\"lag_ms\":"
    "%" PRIu64 ",\"sync_by_time\":true,\"lag_bytes\":%" PRIu64
    ",\"sync_by_bytes\":true,\"lsn\":\"%s\"}",
    replica.snapshot.lag_ms, replica.snapshot.lag_bytes, lsn
  );

  // Act
  TestHTTPResponse response = http_test_get(
    api.port, path, "Accept: text/plain\r\n"
  );

  // Assert
  fixture_pg_status_expect_json(&response, 200, expected_json);

  // Cleanup
  free(expected_json);
  free(lsn);
  free(path);
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_snapshot_consistency(void) {
  // Arrange
  parameters.sync_max_lag_ms = 100;
  parameters.sync_max_lag_bytes = 1000;
  const char *host_name = "changing-host";
  const TestHost first = fixture_pg_status_replica_host(
    host_name, 10, 100, UINT64_C(0x100000010)
  );
  TestHost second = fixture_pg_status_replica_host(
    host_name, 200, 2000, UINT64_C(0x200000020)
  );
  second.snapshot.status.master = true;
  const TestHost hosts[] = {first};
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), -1
  );
  char *path = format_string("/status?host=%s", host_name);
  char *first_lsn = fixture_pg_status_format_expected_lsn(first.snapshot.lsn);
  char *second_lsn = fixture_pg_status_format_expected_lsn(second.snapshot.lsn);
  char *first_json = format_string(
    "{\"master\":false,\"possible_dead\":false,\"alive\":true,\"lag_ms\":"
    "%" PRIu64 ",\"sync_by_time\":true,\"lag_bytes\":%" PRIu64
    ",\"sync_by_bytes\":true,\"lsn\":\"%s\"}",
    first.snapshot.lag_ms, first.snapshot.lag_bytes, first_lsn
  );
  char *second_json = format_string(
    "{\"master\":true,\"possible_dead\":false,\"alive\":true,\"lag_ms\":"
    "%" PRIu64 ",\"sync_by_time\":false,\"lag_bytes\":%" PRIu64
    ",\"sync_by_bytes\":false,\"lsn\":\"%s\"}",
    second.snapshot.lag_ms, second.snapshot.lag_bytes, second_lsn
  );
  PgStatusSnapshotPublisher publisher;
  fixture_pg_status_snapshot_publisher_start(
    &publisher, &monitor_host_list[0], first.snapshot, second.snapshot
  );

  for (size_t i = 0; i < 200; i++) {
    // Act
    TestHTTPResponse response = http_test_get(api.port, path, nullptr);

    // Assert
    http_test_assert_status(&response, 200);
    http_test_assert_contains(&response, "Content-Type: application/json");
    const bool is_consistent =
      fixture_pg_status_json_body_equals(&response, first_json) ||
      fixture_pg_status_json_body_equals(&response, second_json);
    if (!is_consistent) {
      fprintf(
        stderr,
        "Expected either snapshot:\n%s\n%s\nActual response body:\n%s\n",
        first_json, second_json, response.body
      );
      http_test_fail("HTTP API returned a torn snapshot");
    }

    // Cleanup
    http_test_response_free(&response);
  }

  // Cleanup
  fixture_pg_status_snapshot_publisher_stop(&publisher);
  free(second_json);
  free(first_json);
  free(second_lsn);
  free(first_lsn);
  free(path);
  fixture_pg_status_stop(&api);
}

static void test_status_dead(void) {
  // Arrange
  parameters.sync_max_lag_ms = 100;
  parameters.sync_max_lag_bytes = 1000;
  const char *dead_host_name = "dead";
  const TestHost dead = fixture_pg_status_dead_host(dead_host_name);
  const TestHost hosts[] = {
    fixture_pg_status_master_host("master"),
    fixture_pg_status_replica_host("replica", 50, 500, 0x450),
    dead,
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );
  char *path = format_string("/status?host=%s", dead.host);

  // Act
  TestHTTPResponse response = http_test_get(api.port, path, nullptr);

  // Assert
  fixture_pg_status_expect_json(
    &response, 200,
    "{\"master\":false,\"possible_dead\":true,\"alive\":false,\"lag_ms\":null,"
    "\"sync_by_time\":false,\"lag_bytes\":null,"
    "\"sync_by_bytes\":false,\"lsn\":null}"
  );

  // Cleanup
  free(path);
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_status_possible_dead(void) {
  // Arrange
  parameters.sync_max_lag_ms = 100;
  parameters.sync_max_lag_bytes = 1000;
  TestHost possible_dead_replica = fixture_pg_status_possible_dead_replica_host(
    "replica", 50, 500, 0x450
  );
  const TestHost hosts[] = {
    fixture_pg_status_master_host("master"),
    possible_dead_replica,
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );
  char *path = format_string("/status?host=%s", possible_dead_replica.host);
  const char *expected_json = format_string(
    "{\"master\":false,\"alive\":true,\"possible_dead\":true,"
    "\"lag_ms\":%" PRIu64
    ",\"sync_by_time\":true"
    ",\"lag_bytes\":%" PRIu64
    ",\"sync_by_bytes\":true"
    ",\"lsn\":\"%s\"}",
    possible_dead_replica.snapshot.lag_ms,
    possible_dead_replica.snapshot.lag_bytes,
    fixture_pg_status_format_expected_lsn(possible_dead_replica.snapshot.lsn)
  );

  // Act
  TestHTTPResponse response = http_test_get(api.port, path, nullptr);

  // Assert
  fixture_pg_status_expect_json(&response, 200, expected_json);

  // Cleanup
  free(path);
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_missing_master_text(void) {
  // Arrange
  const TestHost hosts[] = {fixture_pg_status_dead_host("dead")};
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), -1
  );

  // Act
  TestHTTPResponse response = http_test_get(api.port, "/master", nullptr);

  // Assert
  fixture_pg_status_expect_text(&response, 404, "");

  // Cleanup
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_missing_master_json(void) {
  // Arrange
  const TestHost hosts[] = {fixture_pg_status_dead_host("dead")};
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), -1
  );

  // Act
  TestHTTPResponse response = http_test_get(
    api.port, "/master", "Accept: application/json\r\n"
  );

  // Assert
  fixture_pg_status_expect_json(&response, 404, "{\"host\":null}");

  // Cleanup
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_missing_replica(void) {
  // Arrange
  const TestHost hosts[] = {fixture_pg_status_dead_host("dead")};
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), -1
  );

  // Act
  TestHTTPResponse response = http_test_get(api.port, "/replica", nullptr);

  // Assert
  fixture_pg_status_expect_text(&response, 404, "");

  // Cleanup
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_empty_topology_replica(void) {
  // Arrange
  PgStatusApiFixture api = fixture_pg_status_start(nullptr, 0, -1);

  // Act
  TestHTTPResponse response = http_test_get(api.port, "/replica", nullptr);

  // Assert
  fixture_pg_status_expect_text(&response, 404, "");

  // Cleanup
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_missing_replica_json(void) {
  // Arrange
  const TestHost hosts[] = {fixture_pg_status_dead_host("dead")};
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), -1
  );

  // Act
  TestHTTPResponse response = http_test_get(
    api.port, "/replica", "Accept: application/json\r\n"
  );

  // Assert
  fixture_pg_status_expect_json(&response, 404, "{\"host\":null}");

  // Cleanup
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_missing_sync_by_time(void) {
  // Arrange
  const TestHost hosts[] = {fixture_pg_status_dead_host("dead")};
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), -1
  );

  // Act
  TestHTTPResponse response = http_test_get(api.port, "/sync_by_time", nullptr);

  // Assert
  fixture_pg_status_expect_text(&response, 404, "");

  // Cleanup
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_missing_sync_by_bytes(void) {
  // Arrange
  const TestHost hosts[] = {fixture_pg_status_dead_host("dead")};
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), -1
  );

  // Act
  TestHTTPResponse response = http_test_get(
    api.port, "/sync_by_bytes", nullptr
  );

  // Assert
  fixture_pg_status_expect_text(&response, 404, "");

  // Cleanup
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_missing_sync_or(void) {
  // Arrange
  const TestHost hosts[] = {fixture_pg_status_dead_host("dead")};
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), -1
  );

  // Act
  TestHTTPResponse response = http_test_get(
    api.port, "/sync_by_time_or_bytes", nullptr
  );

  // Assert
  fixture_pg_status_expect_text(&response, 404, "");

  // Cleanup
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_missing_sync_and(void) {
  // Arrange
  const TestHost hosts[] = {fixture_pg_status_dead_host("dead")};
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), -1
  );

  // Act
  TestHTTPResponse response = http_test_get(
    api.port, "/sync_by_time_and_bytes", nullptr
  );

  // Assert
  fixture_pg_status_expect_text(&response, 404, "");

  // Cleanup
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_missing_most_sync(void) {
  // Arrange
  const TestHost hosts[] = {fixture_pg_status_dead_host("dead")};
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), -1
  );

  // Act
  TestHTTPResponse response = http_test_get(
    api.port, "/most_sync_by_bytes", nullptr
  );

  // Assert
  fixture_pg_status_expect_text(&response, 404, "");

  // Cleanup
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_missing_status_host(void) {
  // Arrange
  const TestHost hosts[] = {fixture_pg_status_dead_host("dead")};
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), -1
  );

  // Act
  TestHTTPResponse response = http_test_get(api.port, "/status", nullptr);

  // Assert
  fixture_pg_status_expect_json(
    &response, 400, "{\"error_text\":\"Get parameter 'host' wasn't passed\"}"
  );

  // Cleanup
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_unknown_status_host(void) {
  // Arrange
  const TestHost hosts[] = {fixture_pg_status_dead_host("dead")};
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), -1
  );

  // Act
  TestHTTPResponse response = http_test_get(
    api.port, "/status?host=unknown", nullptr
  );

  // Assert
  fixture_pg_status_expect_text(&response, 404, "");

  // Cleanup
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_unknown_route(void) {
  // Arrange
  const TestHost hosts[] = {fixture_pg_status_dead_host("dead")};
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), -1
  );

  // Act
  TestHTTPResponse response = http_test_get(api.port, "/unknown", nullptr);

  // Assert
  fixture_pg_status_expect_text(&response, 404, "");

  // Cleanup
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_round_robin(void) {
  // Arrange
  const char *first_replica_host_name = "replica-1";
  const char *second_replica_host_name = "replica-2";
  const TestHost hosts[] = {
    fixture_pg_status_master_host("master"),
    fixture_pg_status_replica_host(first_replica_host_name, 10, 10, 0x490),
    fixture_pg_status_replica_host(second_replica_host_name, 20, 20, 0x480),
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );
  const char *expected[] = {
    first_replica_host_name,
    second_replica_host_name,
    first_replica_host_name,
    second_replica_host_name,
  };

  for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
    // Act
    TestHTTPResponse response = http_test_get(api.port, "/replica", nullptr);

    // Assert
    fixture_pg_status_expect_text(&response, 200, expected[i]);

    // Cleanup
    http_test_response_free(&response);
  }

  // Cleanup
  fixture_pg_status_stop(&api);
}

static void test_round_robin_master_in_middle(void) {
  // Arrange
  const char *before_master_host_name = "before-master";
  const char *after_master_host_name = "after-master";
  const TestHost hosts[] = {
    fixture_pg_status_replica_host(before_master_host_name, 10, 10, 0x490),
    fixture_pg_status_master_host("master"),
    fixture_pg_status_replica_host(after_master_host_name, 20, 20, 0x480),
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 1
  );
  const char *expected[] = {
    after_master_host_name,
    before_master_host_name,
    after_master_host_name,
    before_master_host_name,
  };

  for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
    // Act
    TestHTTPResponse response = http_test_get(api.port, "/replica", nullptr);

    // Assert
    fixture_pg_status_expect_text(&response, 200, expected[i]);

    // Cleanup
    http_test_response_free(&response);
  }

  // Cleanup
  fixture_pg_status_stop(&api);
}

static void test_replica_combined_lag_filters(void) {
  // Arrange
  const char *candidate_host_name = "candidate";
  const TestHost hosts[] = {
    fixture_pg_status_master_host("master"),
    fixture_pg_status_replica_host(candidate_host_name, 50, 100, 0x450),
    fixture_pg_status_replica_host("stale", 500, 5000, 0x300),
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );

  // Act
  TestHTTPResponse response = http_test_get(
    api.port, "/replica?lag_ms=100&lag_bytes=200", nullptr
  );

  // Assert
  fixture_pg_status_expect_text(&response, 200, candidate_host_name);

  // Cleanup
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_replica_lag_ms_fallback(void) {
  // Arrange
  const char *master_host_name = "master";
  const TestHost hosts[] = {
    fixture_pg_status_master_host(master_host_name),
    fixture_pg_status_replica_host("candidate", 50, 100, 0x450),
    fixture_pg_status_replica_host("stale", 500, 5000, 0x300),
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );

  // Act
  TestHTTPResponse response = http_test_get(
    api.port, "/replica?lag_ms=10", nullptr
  );

  // Assert
  fixture_pg_status_expect_text(&response, 200, master_host_name);

  // Cleanup
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_replica_lag_bytes_fallback(void) {
  // Arrange
  const char *master_host_name = "master";
  const TestHost hosts[] = {
    fixture_pg_status_master_host(master_host_name),
    fixture_pg_status_replica_host("candidate", 50, 100, 0x450),
    fixture_pg_status_replica_host("stale", 500, 5000, 0x300),
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );

  // Act
  TestHTTPResponse response = http_test_get(
    api.port, "/replica?lag_bytes=50", nullptr
  );

  // Assert
  fixture_pg_status_expect_text(&response, 200, master_host_name);

  // Cleanup
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_replica_min_lsn_match(void) {
  // Arrange
  const char *candidate_host_name = "candidate";
  const TestHost hosts[] = {
    fixture_pg_status_master_host("master"),
    fixture_pg_status_replica_host(candidate_host_name, 50, 100, 0x450),
    fixture_pg_status_replica_host("stale", 500, 5000, 0x300),
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );

  // Act
  TestHTTPResponse response = http_test_get(
    api.port, "/replica?min_lsn=0/400", nullptr
  );

  // Assert
  fixture_pg_status_expect_text(&response, 200, candidate_host_name);

  // Cleanup
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_replica_min_lsn_fallback(void) {
  // Arrange
  const char *master_host_name = "master";
  const TestHost hosts[] = {
    fixture_pg_status_master_host(master_host_name),
    fixture_pg_status_replica_host("candidate", 50, 100, 0x450),
    fixture_pg_status_replica_host("stale", 500, 5000, 0x300),
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );

  // Act
  TestHTTPResponse response = http_test_get(
    api.port, "/replica?min_lsn=0/460", nullptr
  );

  // Assert
  fixture_pg_status_expect_text(&response, 200, master_host_name);

  // Cleanup
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_sync_by_time_selects_candidate(void) {
  // Arrange
  parameters.sync_max_lag_ms = 100;
  parameters.sync_max_lag_bytes = 1000;
  const char *candidate_host_name = "candidate";
  const TestHost hosts[] = {
    fixture_pg_status_master_host("master"),
    fixture_pg_status_replica_host(candidate_host_name, 50, 2000, 0x450),
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );

  // Act
  TestHTTPResponse response = http_test_get(api.port, "/sync_by_time", nullptr);

  // Assert
  fixture_pg_status_expect_text(&response, 200, candidate_host_name);

  // Cleanup
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_sync_by_bytes_falls_back(void) {
  // Arrange
  parameters.sync_max_lag_ms = 100;
  parameters.sync_max_lag_bytes = 1000;
  const char *master_host_name = "master";
  const TestHost hosts[] = {
    fixture_pg_status_master_host(master_host_name),
    fixture_pg_status_replica_host("candidate", 50, 2000, 0x450),
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );

  // Act
  TestHTTPResponse response = http_test_get(
    api.port, "/sync_by_bytes", nullptr
  );

  // Assert
  fixture_pg_status_expect_text(&response, 200, master_host_name);

  // Cleanup
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_sync_or_time_match(void) {
  // Arrange
  parameters.sync_max_lag_ms = 100;
  parameters.sync_max_lag_bytes = 1000;
  const char *candidate_host_name = "candidate";
  const TestHost hosts[] = {
    fixture_pg_status_master_host("master"),
    fixture_pg_status_replica_host(candidate_host_name, 50, 2000, 0x450),
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );

  // Act
  TestHTTPResponse response = http_test_get(
    api.port, "/sync_by_time_or_bytes", nullptr
  );

  // Assert
  fixture_pg_status_expect_text(&response, 200, candidate_host_name);

  // Cleanup
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_sync_and_rejects_time_only(void) {
  // Arrange
  parameters.sync_max_lag_ms = 100;
  parameters.sync_max_lag_bytes = 1000;
  const char *master_host_name = "master";
  const TestHost hosts[] = {
    fixture_pg_status_master_host(master_host_name),
    fixture_pg_status_replica_host("candidate", 50, 2000, 0x450),
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );

  // Act
  TestHTTPResponse response = http_test_get(
    api.port, "/sync_by_time_and_bytes", nullptr
  );

  // Assert
  fixture_pg_status_expect_text(&response, 200, master_host_name);

  // Cleanup
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_sync_or_bytes_match(void) {
  // Arrange
  parameters.sync_max_lag_ms = 100;
  parameters.sync_max_lag_bytes = 1000;
  const char *candidate_host_name = "candidate";
  const TestHost hosts[] = {
    fixture_pg_status_master_host("master"),
    fixture_pg_status_replica_host(candidate_host_name, 200, 100, 0x450),
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );

  // Act
  TestHTTPResponse response = http_test_get(
    api.port, "/sync_by_time_or_bytes", nullptr
  );

  // Assert
  fixture_pg_status_expect_text(&response, 200, candidate_host_name);

  // Cleanup
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_sync_and_rejects_bytes_only(void) {
  // Arrange
  parameters.sync_max_lag_ms = 100;
  parameters.sync_max_lag_bytes = 1000;
  const char *master_host_name = "master";
  const TestHost hosts[] = {
    fixture_pg_status_master_host(master_host_name),
    fixture_pg_status_replica_host("candidate", 200, 100, 0x450),
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );

  // Act
  TestHTTPResponse response = http_test_get(
    api.port, "/sync_by_time_and_bytes", nullptr
  );

  // Assert
  fixture_pg_status_expect_text(&response, 200, master_host_name);

  // Cleanup
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_sync_and_threshold_boundary(void) {
  // Arrange
  const uint64_t max_lag_ms = 100;
  const uint64_t max_lag_bytes = 1000;
  parameters.sync_max_lag_ms = max_lag_ms;
  parameters.sync_max_lag_bytes = max_lag_bytes;
  const char *candidate_host_name = "candidate";
  const TestHost hosts[] = {
    fixture_pg_status_master_host("master"),
    fixture_pg_status_replica_host(
      candidate_host_name, max_lag_ms, max_lag_bytes, 0x450
    ),
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );

  // Act
  TestHTTPResponse response = http_test_get(
    api.port, "/sync_by_time_and_bytes", nullptr
  );

  // Assert
  fixture_pg_status_expect_text(&response, 200, candidate_host_name);

  // Cleanup
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_sync_or_query_overrides_globals(void) {
  // Arrange
  parameters.sync_max_lag_ms = 100;
  parameters.sync_max_lag_bytes = 1000;
  const char *candidate_host_name = "candidate";
  const TestHost candidate = fixture_pg_status_replica_host(
    candidate_host_name, 200, 2000, 0x450
  );
  const TestHost hosts[] = {
    fixture_pg_status_master_host("master"),
    candidate,
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );
  char *path = format_string(
    "/sync_by_time_or_bytes?lag_ms=%" PRIu64, candidate.snapshot.lag_ms
  );

  // Act
  TestHTTPResponse response = http_test_get(api.port, path, nullptr);

  // Assert
  fixture_pg_status_expect_text(&response, 200, candidate_host_name);

  // Cleanup
  free(path);
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_sync_and_query_overrides_globals(void) {
  // Arrange
  parameters.sync_max_lag_ms = 100;
  parameters.sync_max_lag_bytes = 1000;
  const char *candidate_host_name = "candidate";
  const TestHost candidate = fixture_pg_status_replica_host(
    candidate_host_name, 200, 2000, 0x450
  );
  const TestHost hosts[] = {
    fixture_pg_status_master_host("master"),
    candidate,
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );
  char *path = format_string(
    "/sync_by_time_and_bytes?lag_ms=%" PRIu64 "&lag_bytes=%" PRIu64,
    candidate.snapshot.lag_ms, candidate.snapshot.lag_bytes
  );

  // Act
  TestHTTPResponse response = http_test_get(api.port, path, nullptr);

  // Assert
  fixture_pg_status_expect_text(&response, 200, candidate_host_name);

  // Cleanup
  free(path);
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_sync_by_time_ignores_lag_bytes(void) {
  // Arrange
  parameters.sync_max_lag_ms = 100;
  parameters.sync_max_lag_bytes = 1000;
  const char *master_host_name = "master";
  const TestHost hosts[] = {
    fixture_pg_status_master_host(master_host_name),
    fixture_pg_status_replica_host("candidate", 100, 1000, 0x450),
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );

  // Act
  TestHTTPResponse response = http_test_get(
    api.port, "/sync_by_time?lag_ms=40&lag_bytes=18446744073709551615", nullptr
  );

  // Assert
  fixture_pg_status_expect_text(&response, 200, master_host_name);

  // Cleanup
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_sync_by_bytes_ignores_lag_ms(void) {
  // Arrange
  parameters.sync_max_lag_ms = 100;
  parameters.sync_max_lag_bytes = 1000;
  const char *candidate_host_name = "candidate";
  const TestHost hosts[] = {
    fixture_pg_status_master_host("master"),
    fixture_pg_status_replica_host(candidate_host_name, 100, 1000, 0x450),
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );

  // Act
  TestHTTPResponse response = http_test_get(
    api.port, "/sync_by_bytes?lag_ms=0&lag_bytes=2000", nullptr
  );

  // Assert
  fixture_pg_status_expect_text(&response, 200, candidate_host_name);

  // Cleanup
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_sync_by_time_min_lsn(void) {
  // Arrange
  parameters.sync_max_lag_ms = 100;
  parameters.sync_max_lag_bytes = 1000;
  const char *master_host_name = "master";
  const TestHost hosts[] = {
    fixture_pg_status_master_host(master_host_name),
    fixture_pg_status_replica_host("candidate", 100, 1000, 0x450),
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );

  // Act
  TestHTTPResponse response = http_test_get(
    api.port, "/sync_by_time?min_lsn=0/460", nullptr
  );

  // Assert
  fixture_pg_status_expect_text(&response, 200, master_host_name);

  // Cleanup
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_sync_by_bytes_min_lsn(void) {
  // Arrange
  parameters.sync_max_lag_ms = 100;
  parameters.sync_max_lag_bytes = 1000;
  const char *replica_host_name = "replica";
  const uint64_t replica_lsn = 0x450;
  const TestHost hosts[] = {
    fixture_pg_status_master_host("master"),
    fixture_pg_status_replica_host(replica_host_name, 500, 100, replica_lsn),
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );
  char *minimum_lsn = fixture_pg_status_format_expected_lsn(replica_lsn);
  char *path = format_string("/sync_by_bytes?min_lsn=%s", minimum_lsn);

  // Act
  TestHTTPResponse response = http_test_get(api.port, path, nullptr);

  // Assert
  fixture_pg_status_expect_text(&response, 200, replica_host_name);

  // Cleanup
  free(path);
  free(minimum_lsn);
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_sync_or_min_lsn(void) {
  // Arrange
  parameters.sync_max_lag_ms = 100;
  parameters.sync_max_lag_bytes = 1000;
  const char *master_host_name = "master";
  const uint64_t replica_lsn = 0x450;
  const uint64_t minimum_lsn_value = replica_lsn + 1;
  const TestHost hosts[] = {
    fixture_pg_status_master_host(master_host_name),
    fixture_pg_status_replica_host("replica", 50, 2000, replica_lsn),
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );
  char *minimum_lsn = fixture_pg_status_format_expected_lsn(minimum_lsn_value);
  char *path = format_string("/sync_by_time_or_bytes?min_lsn=%s", minimum_lsn);

  // Act
  TestHTTPResponse response = http_test_get(api.port, path, nullptr);

  // Assert
  fixture_pg_status_expect_text(&response, 200, master_host_name);

  // Cleanup
  free(path);
  free(minimum_lsn);
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_most_sync_by_bytes(void) {
  // Arrange
  parameters.sync_max_lag_ms = 1000;
  parameters.sync_max_lag_bytes = 1000;
  const char *closest_replica_host_name = "replica-50";
  const TestHost hosts[] = {
    fixture_pg_status_master_host("master"),
    fixture_pg_status_replica_host("replica-100", 50, 100, 0x450),
    fixture_pg_status_replica_host(closest_replica_host_name, 500, 50, 0x350),
    fixture_pg_status_replica_host("replica-200", 10, 200, 0x490),
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );

  // Act
  TestHTTPResponse response = http_test_get(
    api.port, "/most_sync_by_bytes", nullptr
  );

  // Assert
  fixture_pg_status_expect_text(&response, 200, closest_replica_host_name);

  // Cleanup
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_most_sync_lag_bytes_filter(void) {
  // Arrange
  parameters.sync_max_lag_ms = 1000;
  parameters.sync_max_lag_bytes = 1000;
  const char *master_host_name = "master";
  const TestHost hosts[] = {
    fixture_pg_status_master_host(master_host_name),
    fixture_pg_status_replica_host("replica-100", 50, 100, 0x450),
    fixture_pg_status_replica_host("replica-50", 500, 50, 0x350),
    fixture_pg_status_replica_host("replica-200", 10, 200, 0x490),
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );

  // Act
  TestHTTPResponse response = http_test_get(
    api.port, "/most_sync_by_bytes?lag_bytes=40", nullptr
  );

  // Assert
  fixture_pg_status_expect_text(&response, 200, master_host_name);

  // Cleanup
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_most_sync_min_lsn_filter(void) {
  // Arrange
  parameters.sync_max_lag_ms = 1000;
  parameters.sync_max_lag_bytes = 1000;
  const char *expected_replica_host_name = "replica-100";
  const TestHost hosts[] = {
    fixture_pg_status_master_host("master"),
    fixture_pg_status_replica_host(expected_replica_host_name, 50, 100, 0x450),
    fixture_pg_status_replica_host("replica-50", 500, 50, 0x350),
    fixture_pg_status_replica_host("replica-200", 10, 200, 0x490),
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );

  // Act
  TestHTTPResponse response = http_test_get(
    api.port, "/most_sync_by_bytes?min_lsn=0/400", nullptr
  );

  // Assert
  fixture_pg_status_expect_text(&response, 200, expected_replica_host_name);

  // Cleanup
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_most_sync_ignores_lag_ms(void) {
  // Arrange
  parameters.sync_max_lag_ms = 1000;
  parameters.sync_max_lag_bytes = 1000;
  const char *closest_replica_host_name = "replica-50";
  const TestHost hosts[] = {
    fixture_pg_status_master_host("master"),
    fixture_pg_status_replica_host("replica-100", 50, 100, 0x450),
    fixture_pg_status_replica_host(closest_replica_host_name, 500, 50, 0x350),
    fixture_pg_status_replica_host("replica-200", 10, 200, 0x490),
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );

  // Act
  TestHTTPResponse response = http_test_get(
    api.port, "/most_sync_by_bytes?lag_ms=0", nullptr
  );

  // Assert
  fixture_pg_status_expect_text(&response, 200, closest_replica_host_name);

  // Cleanup
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_replica_prefers_alive(void) {
  // Arrange
  parameters.sync_max_lag_ms = 1000;
  parameters.sync_max_lag_bytes = 1000;
  const char *alive_host_name = "alive";
  const TestHost hosts[] = {
    fixture_pg_status_master_host("master"),
    fixture_pg_status_possible_dead_replica_host("possible", 10, 10, 0x490),
    fixture_pg_status_replica_host(alive_host_name, 20, 20, 0x480),
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );

  // Act
  TestHTTPResponse response = http_test_get(api.port, "/replica", nullptr);

  // Assert
  fixture_pg_status_expect_text(&response, 200, alive_host_name);

  // Cleanup
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_most_sync_prefers_alive(void) {
  // Arrange
  parameters.sync_max_lag_ms = 1000;
  parameters.sync_max_lag_bytes = 1000;
  const char *alive_host_name = "alive";
  const TestHost hosts[] = {
    fixture_pg_status_master_host("master"),
    fixture_pg_status_possible_dead_replica_host("possible", 10, 10, 0x490),
    fixture_pg_status_replica_host(alive_host_name, 20, 20, 0x480),
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );

  // Act
  TestHTTPResponse response = http_test_get(
    api.port, "/most_sync_by_bytes", nullptr
  );

  // Assert
  fixture_pg_status_expect_text(&response, 200, alive_host_name);

  // Cleanup
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_replica_uses_possible_dead(void) {
  // Arrange
  parameters.sync_max_lag_ms = 1000;
  parameters.sync_max_lag_bytes = 1000;
  const char *possible_dead_host_name = "possible";
  const TestHost hosts[] = {
    fixture_pg_status_master_host("master"),
    fixture_pg_status_possible_dead_replica_host(
      possible_dead_host_name, 10, 10, 0x490
    ),
    fixture_pg_status_dead_host("dead"),
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );

  // Act
  TestHTTPResponse response = http_test_get(api.port, "/replica", nullptr);

  // Assert
  fixture_pg_status_expect_text(&response, 200, possible_dead_host_name);

  // Cleanup
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_most_sync_uses_possible_dead(void) {
  // Arrange
  parameters.sync_max_lag_ms = 1000;
  parameters.sync_max_lag_bytes = 1000;
  const char *possible_dead_host_name = "possible";
  const TestHost hosts[] = {
    fixture_pg_status_master_host("master"),
    fixture_pg_status_possible_dead_replica_host(
      possible_dead_host_name, 10, 10, 0x490
    ),
    fixture_pg_status_dead_host("dead"),
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );

  // Act
  TestHTTPResponse response = http_test_get(
    api.port, "/most_sync_by_bytes", nullptr
  );

  // Assert
  fixture_pg_status_expect_text(&response, 200, possible_dead_host_name);

  // Cleanup
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_most_sync_tie_break(void) {
  // Arrange
  parameters.sync_max_lag_bytes = 1000;
  const char *first_replica_host_name = "first";
  const TestHost hosts[] = {
    fixture_pg_status_master_host("master"),
    fixture_pg_status_replica_host(first_replica_host_name, 200, 100, 0x400),
    fixture_pg_status_replica_host("second", 10, 100, 0x490),
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );

  // Act
  TestHTTPResponse response = http_test_get(
    api.port, "/most_sync_by_bytes", nullptr
  );

  // Assert
  fixture_pg_status_expect_text(&response, 200, first_replica_host_name);

  // Cleanup
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_replica_lsn_boundary(void) {
  // Arrange
  parameters.sync_max_lag_ms = 100;
  parameters.sync_max_lag_bytes = 1000;
  const char *replica_host_name = "replica";
  const uint64_t replica_lsn = 0x450;
  const TestHost hosts[] = {
    fixture_pg_status_master_host("master"),
    fixture_pg_status_replica_host(replica_host_name, 50, 100, replica_lsn),
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );
  char *minimum_lsn = fixture_pg_status_format_expected_lsn(replica_lsn);
  char *path = format_string("/replica?min_lsn=%s", minimum_lsn);

  // Act
  TestHTTPResponse response = http_test_get(api.port, path, nullptr);

  // Assert
  fixture_pg_status_expect_text(&response, 200, replica_host_name);

  // Cleanup
  free(path);
  free(minimum_lsn);
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_replica_high_lsn_boundary(void) {
  // Arrange
  const char *replica_host_name = "replica";
  const uint64_t replica_lsn = UINT64_C(1) << 32;
  const TestHost hosts[] = {
    fixture_pg_status_master_host("master"),
    fixture_pg_status_replica_host(replica_host_name, 50, 100, replica_lsn),
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );
  char *minimum_lsn = fixture_pg_status_format_expected_lsn(replica_lsn);
  char *path = format_string("/replica?min_lsn=%s", minimum_lsn);

  // Act
  TestHTTPResponse response = http_test_get(api.port, path, nullptr);

  // Assert
  fixture_pg_status_expect_text(&response, 200, replica_host_name);

  // Cleanup
  free(path);
  free(minimum_lsn);
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_sync_and_lsn_boundary(void) {
  // Arrange
  parameters.sync_max_lag_ms = 100;
  parameters.sync_max_lag_bytes = 1000;
  const char *replica_host_name = "replica";
  const uint64_t replica_lsn = 0x450;
  const TestHost hosts[] = {
    fixture_pg_status_master_host("master"),
    fixture_pg_status_replica_host(replica_host_name, 50, 100, replica_lsn),
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );
  char *minimum_lsn = fixture_pg_status_format_expected_lsn(replica_lsn);
  char *path = format_string("/sync_by_time_and_bytes?min_lsn=%s", minimum_lsn);

  // Act
  TestHTTPResponse response = http_test_get(api.port, path, nullptr);

  // Assert
  fixture_pg_status_expect_text(&response, 200, replica_host_name);

  // Cleanup
  free(path);
  free(minimum_lsn);
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_most_sync_lsn_boundary(void) {
  // Arrange
  parameters.sync_max_lag_ms = 100;
  parameters.sync_max_lag_bytes = 1000;
  const char *replica_host_name = "replica";
  const uint64_t replica_lsn = 0x450;
  const TestHost hosts[] = {
    fixture_pg_status_master_host("master"),
    fixture_pg_status_replica_host(replica_host_name, 50, 100, replica_lsn),
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );
  char *minimum_lsn = fixture_pg_status_format_expected_lsn(replica_lsn);
  char *path = format_string("/most_sync_by_bytes?min_lsn=%s", minimum_lsn);

  // Act
  TestHTTPResponse response = http_test_get(api.port, path, nullptr);

  // Assert
  fixture_pg_status_expect_text(&response, 200, replica_host_name);

  // Cleanup
  free(path);
  free(minimum_lsn);
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_replica_lsn_above_boundary(void) {
  // Arrange
  parameters.sync_max_lag_ms = 100;
  parameters.sync_max_lag_bytes = 1000;
  const char *master_host_name = "master";
  const uint64_t replica_lsn = 0x450;
  const uint64_t minimum_lsn_value = replica_lsn + 1;
  const TestHost hosts[] = {
    fixture_pg_status_master_host(master_host_name),
    fixture_pg_status_replica_host("replica", 50, 100, replica_lsn),
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );
  char *minimum_lsn = fixture_pg_status_format_expected_lsn(minimum_lsn_value);
  char *path = format_string("/replica?min_lsn=%s", minimum_lsn);

  // Act
  TestHTTPResponse response = http_test_get(api.port, path, nullptr);

  // Assert
  fixture_pg_status_expect_text(&response, 200, master_host_name);

  // Cleanup
  free(path);
  free(minimum_lsn);
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_master_switch_master(void) {
  // Arrange
  parameters.sync_max_lag_ms = 100;
  parameters.sync_max_lag_bytes = 1000;
  const char *old_master_host_name = "master";
  const char *new_master_host_name = "new-master";
  const TestHost initial_hosts[] = {
    fixture_pg_status_master_host(old_master_host_name),
    fixture_pg_status_replica_host(new_master_host_name, 20, 20, 0x480),
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    initial_hosts, sizeof(initial_hosts) / sizeof(initial_hosts[0]), 0
  );

  const TestHost old_master = fixture_pg_status_replica_host(
    old_master_host_name, 10, 10, 0x510
  );
  publish_monitor_snapshot(&monitor_host_list[0], old_master.snapshot);
  TestHost new_master = fixture_pg_status_master_host(new_master_host_name);
  new_master.snapshot.lsn = 0x520;
  publish_monitor_snapshot(&monitor_host_list[1], new_master.snapshot);
  save_master_index(1);

  // Act
  TestHTTPResponse response = http_test_get(api.port, "/master", nullptr);

  // Assert
  fixture_pg_status_expect_text(&response, 200, new_master_host_name);

  // Cleanup
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_master_switch_replica(void) {
  // Arrange
  parameters.sync_max_lag_ms = 100;
  parameters.sync_max_lag_bytes = 1000;
  const char *old_master_host_name = "master";
  const char *new_master_host_name = "new-master";
  const TestHost initial_hosts[] = {
    fixture_pg_status_master_host(old_master_host_name),
    fixture_pg_status_replica_host(new_master_host_name, 20, 20, 0x480),
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    initial_hosts, sizeof(initial_hosts) / sizeof(initial_hosts[0]), 0
  );

  const TestHost old_master = fixture_pg_status_replica_host(
    old_master_host_name, 10, 10, 0x510
  );
  publish_monitor_snapshot(&monitor_host_list[0], old_master.snapshot);
  TestHost new_master = fixture_pg_status_master_host(new_master_host_name);
  new_master.snapshot.lsn = 0x520;
  publish_monitor_snapshot(&monitor_host_list[1], new_master.snapshot);
  save_master_index(1);

  // Act
  TestHTTPResponse response = http_test_get(api.port, "/replica", nullptr);

  // Assert
  fixture_pg_status_expect_text(&response, 200, old_master_host_name);

  // Cleanup
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_master_switch_hosts(void) {
  // Arrange
  parameters.sync_max_lag_ms = 100;
  parameters.sync_max_lag_bytes = 1000;
  const char *old_master_host_name = "master";
  const char *new_master_host_name = "new-master";
  const TestHost initial_hosts[] = {
    fixture_pg_status_master_host(old_master_host_name),
    fixture_pg_status_replica_host(new_master_host_name, 20, 20, 0x480),
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    initial_hosts, sizeof(initial_hosts) / sizeof(initial_hosts[0]), 0
  );

  const TestHost old_master = fixture_pg_status_replica_host(
    old_master_host_name, 10, 10, 0x510
  );
  publish_monitor_snapshot(&monitor_host_list[0], old_master.snapshot);
  TestHost new_master = fixture_pg_status_master_host(new_master_host_name);
  new_master.snapshot.lsn = 0x520;
  publish_monitor_snapshot(&monitor_host_list[1], new_master.snapshot);
  save_master_index(1);
  char *old_master_lsn = fixture_pg_status_format_expected_lsn(
    old_master.snapshot.lsn
  );
  char *new_master_lsn = fixture_pg_status_format_expected_lsn(
    new_master.snapshot.lsn
  );
  char *expected_json = format_string(
    "["
    "{\"host\":\"%s\",\"master\":false,\"possible_dead\":false,\"alive\":true,"
    "\"lag_ms\":%" PRIu64
    ",\"sync_by_time\":true,"
    "\"lag_bytes\":%" PRIu64
    ",\"sync_by_bytes\":true,\"lsn\":\"%s\"},"
    "{\"host\":\"%s\",\"master\":true,\"possible_dead\":false,\"alive\":true,"
    "\"lag_ms\":%" PRIu64
    ",\"sync_by_time\":true,"
    "\"lag_bytes\":%" PRIu64
    ",\"sync_by_bytes\":true,\"lsn\":\"%s\"}"
    "]",
    old_master.host, old_master.snapshot.lag_ms, old_master.snapshot.lag_bytes,
    old_master_lsn, new_master.host, new_master.snapshot.lag_ms,
    new_master.snapshot.lag_bytes, new_master_lsn
  );

  // Act
  TestHTTPResponse response = http_test_get(api.port, "/hosts", nullptr);

  // Assert
  fixture_pg_status_expect_json(&response, 200, expected_json);

  // Cleanup
  free(expected_json);
  free(new_master_lsn);
  free(old_master_lsn);
  http_test_response_free(&response);
  fixture_pg_status_stop(&api);
}

static void test_invalid_parameters(void) {
  // Arrange
  const TestHost hosts[] = {
    fixture_pg_status_master_host("master"),
    fixture_pg_status_replica_host("replica", 50, 100, 0x450),
  };
  PgStatusApiFixture api = fixture_pg_status_start(
    hosts, sizeof(hosts) / sizeof(hosts[0]), 0
  );
  const struct {
    const char *path;
    const char *error;
  } cases[] = {
    {"/replica?lag_ms=-1", "Invalid lag_ms"},
    {"/replica?lag_ms=", "Invalid lag_ms"},
    {"/sync_by_time?lag_ms=text", "Invalid lag_ms"},
    {"/sync_by_bytes?lag_bytes=18446744073709551616", "Invalid lag_bytes"},
    {"/sync_by_time_or_bytes?lag_bytes=1x", "Invalid lag_bytes"},
    {"/sync_by_time_and_bytes?min_lsn=invalid", "Invalid min_lsn"},
    {"/most_sync_by_bytes?min_lsn=100", "Invalid min_lsn"},
  };

  // All rows exercise the same parameter-validation contract.
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    char expected[128];
    const int length = snprintf(
      expected, sizeof(expected), "{\"error_text\":\"%s\"}", cases[i].error
    );
    http_test_assert_true(
      length > 0 && (size_t)length < sizeof(expected), "error JSON format"
    );

    // Act
    TestHTTPResponse response = http_test_get(api.port, cases[i].path, nullptr);

    // Assert
    fixture_pg_status_expect_json(&response, 400, expected);

    // Cleanup
    http_test_response_free(&response);
  }

  // Cleanup
  fixture_pg_status_stop(&api);
}

typedef void (*test_function_t)(void);

static const struct {
  const char *name;
  test_function_t function;
} test_cases[] = {
  {"version", test_version},
  {"version_ignores_accept", test_version_ignores_accept},
  {"master_text", test_master_text},
  {"master_json", test_master_json},
  {"replica_json", test_replica_json},
  {"hosts", test_hosts},
  {"hosts_ignores_accept", test_hosts_ignores_accept},
  {"status_alive", test_status_alive},
  {"status_mixed_sync", test_status_mixed_sync},
  {"status_max_lsn", test_status_max_lsn},
  {"status_ignores_accept", test_status_ignores_accept},
  {"snapshot_consistency", test_snapshot_consistency},
  {"status_dead", test_status_dead},
  {"status_possible_dead", test_status_possible_dead},
  {"missing_master_text", test_missing_master_text},
  {"missing_master_json", test_missing_master_json},
  {"missing_replica", test_missing_replica},
  {"empty_topology_replica", test_empty_topology_replica},
  {"missing_replica_json", test_missing_replica_json},
  {"missing_sync_by_time", test_missing_sync_by_time},
  {"missing_sync_by_bytes", test_missing_sync_by_bytes},
  {"missing_sync_or", test_missing_sync_or},
  {"missing_sync_and", test_missing_sync_and},
  {"missing_most_sync", test_missing_most_sync},
  {"missing_status_host", test_missing_status_host},
  {"unknown_status_host", test_unknown_status_host},
  {"unknown_route", test_unknown_route},
  {"round_robin", test_round_robin},
  {"round_robin_master_in_middle", test_round_robin_master_in_middle},
  {"replica_combined_lag_filters", test_replica_combined_lag_filters},
  {"replica_lag_ms_fallback", test_replica_lag_ms_fallback},
  {"replica_lag_bytes_fallback", test_replica_lag_bytes_fallback},
  {"replica_min_lsn_match", test_replica_min_lsn_match},
  {"replica_min_lsn_fallback", test_replica_min_lsn_fallback},
  {"sync_by_time_selects_candidate", test_sync_by_time_selects_candidate},
  {"sync_by_bytes_falls_back", test_sync_by_bytes_falls_back},
  {"sync_or_time_match", test_sync_or_time_match},
  {"sync_and_rejects_time_only", test_sync_and_rejects_time_only},
  {"sync_or_bytes_match", test_sync_or_bytes_match},
  {"sync_and_rejects_bytes_only", test_sync_and_rejects_bytes_only},
  {"sync_and_threshold_boundary", test_sync_and_threshold_boundary},
  {"sync_or_query_overrides_globals", test_sync_or_query_overrides_globals},
  {"sync_and_query_overrides_globals", test_sync_and_query_overrides_globals},
  {"sync_by_time_ignores_lag_bytes", test_sync_by_time_ignores_lag_bytes},
  {"sync_by_bytes_ignores_lag_ms", test_sync_by_bytes_ignores_lag_ms},
  {"sync_by_time_min_lsn", test_sync_by_time_min_lsn},
  {"sync_by_bytes_min_lsn", test_sync_by_bytes_min_lsn},
  {"sync_or_min_lsn", test_sync_or_min_lsn},
  {"most_sync_by_bytes", test_most_sync_by_bytes},
  {"most_sync_lag_bytes_filter", test_most_sync_lag_bytes_filter},
  {"most_sync_min_lsn_filter", test_most_sync_min_lsn_filter},
  {"most_sync_ignores_lag_ms", test_most_sync_ignores_lag_ms},
  {"replica_prefers_alive", test_replica_prefers_alive},
  {"most_sync_prefers_alive", test_most_sync_prefers_alive},
  {"replica_uses_possible_dead", test_replica_uses_possible_dead},
  {"most_sync_uses_possible_dead", test_most_sync_uses_possible_dead},
  {"most_sync_tie_break", test_most_sync_tie_break},
  {"replica_lsn_boundary", test_replica_lsn_boundary},
  {"replica_high_lsn_boundary", test_replica_high_lsn_boundary},
  {"sync_and_lsn_boundary", test_sync_and_lsn_boundary},
  {"most_sync_lsn_boundary", test_most_sync_lsn_boundary},
  {"replica_lsn_above_boundary", test_replica_lsn_above_boundary},
  {"master_switch_master", test_master_switch_master},
  {"master_switch_replica", test_master_switch_replica},
  {"master_switch_hosts", test_master_switch_hosts},
  {"invalid_parameters", test_invalid_parameters},
};

int main(const int argc, char **argv) {
  if (argc != 2) {
    http_test_fail("expected one test case name");
  }

  for (size_t i = 0; i < sizeof(test_cases) / sizeof(test_cases[0]); i++) {
    if (strcmp(argv[1], test_cases[i].name) == 0) {
      test_cases[i].function();
      printf("pg_status_api_test %s passed\n", argv[1]);
      return EXIT_SUCCESS;
    }
  }

  http_test_fail("unknown test case name");
}
