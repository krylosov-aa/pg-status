#ifndef FIXTURE_PG_STATUS_H
#define FIXTURE_PG_STATUS_H

#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "http_test.h"
#include "pg_monitor.h"
#include "pg_status_api.h"

typedef struct {
  const char *host;
  MonitorSnapshot snapshot;
} TestHost;

typedef struct {
  HTTPServer *server;
  uint16_t port;
} PgStatusApiFixture;

typedef struct {
  pthread_t thread;
  atomic_bool running;
  MonitorHost *host;
  MonitorSnapshot first;
  MonitorSnapshot second;
} PgStatusSnapshotPublisher;

TestHost fixture_pg_status_master_host(const char *name);
TestHost fixture_pg_status_replica_host(
  const char *name, uint64_t lag_ms, uint64_t lag_bytes, uint64_t lsn
);
TestHost fixture_pg_status_possible_dead_replica_host(
  const char *name, uint64_t lag_ms, uint64_t lag_bytes, uint64_t lsn
);
TestHost fixture_pg_status_dead_host(const char *name);
char *fixture_pg_status_format_expected_lsn(uint64_t lsn);
void fixture_pg_status_snapshot_publisher_start(
  PgStatusSnapshotPublisher *publisher, MonitorHost *host,
  MonitorSnapshot first, MonitorSnapshot second
);
void fixture_pg_status_snapshot_publisher_stop(
  PgStatusSnapshotPublisher *publisher
);

PgStatusApiFixture fixture_pg_status_start(
  const TestHost *hosts, size_t count, int master_index
);
void fixture_pg_status_stop(PgStatusApiFixture *api);

void fixture_pg_status_assert_json_body(
  const TestHTTPResponse *response, const char *expected_json
);
bool fixture_pg_status_json_body_equals(
  const TestHTTPResponse *response, const char *expected_json
);
void fixture_pg_status_expect_text(
  const TestHTTPResponse *response, unsigned int status, const char *body
);
void fixture_pg_status_expect_json(
  const TestHTTPResponse *response, unsigned int status,
  const char *expected_json
);

#endif  // FIXTURE_PG_STATUS_H
