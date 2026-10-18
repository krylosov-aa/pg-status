/** Master selection from published host states, including role changes. */

#include <stdlib.h>
#include <string.h>

#include "common_support.h"

// Declared before the observer below renames the calls in pg_monitor.c.
#include "pg_monitor.h"

static void observed_save_master_index(int i);

// Exercise the actual private recomputation without exposing a test-only API.
// Selections go through an observer that records the published state.
#define save_master_index observed_save_master_index
// NOLINTNEXTLINE(bugprone-suspicious-include)
#include "../../src/pg_monitor/pg_monitor.c"
#undef save_master_index

/** State of host B when a master index was last published. */
static MonitorSnapshot b_at_selection;

static void observed_save_master_index(const int i) {
  b_at_selection = atomic_get_snapshot(&monitor_host_list[1]);
  save_master_index(i);
}

static const MonitorStatus healthy_master = {.alive = true, .master = true};
static const MonitorStatus possible_master = {
  .alive = true, .master = true, .possible_dead = true
};
static const MonitorStatus replica = {.alive = true};
static const MonitorStatus dead_host = {.possible_dead = true};

static void set_status(unsigned int index, MonitorStatus status) {
  publish_monitor_snapshot(
    &monitor_host_list[index], (MonitorSnapshot){.status = status}
  );
}

static void configure_hosts(void) {
  host_count = 3;
  parameters.dc_locality_enabled = false;
  parameters.geo_locality_enabled = false;
  const char *names[] = {"A", "B", "C"};
  for (unsigned int i = 0; i < host_count; i++) {
    monitor_host_list[i] = (MonitorHost){.host = names[i]};
    set_status(i, replica);
  }
  save_master_index(-1);
}

static void expect_master(int expected_index) {
  // Act
  recompute_master_index();

  // Assert
  support_assert_true(get_master_index() == expected_index, "master index");
  const MonitorHost *expected = expected_index >= 0
                                  ? &monitor_host_list[expected_index]
                                  : nullptr;
  support_assert_true(get_master_monitor_host() == expected, "master lookup");
  // All fixture replay positions are zero: exclude replicas to exercise the
  // routing fallback using the recomputed index rather than setting it by hand.
  const LagThresholds thresholds = {.min_lsn = UINT64_MAX};
  support_assert_true(
    find_replica(is_alive_replica, &thresholds, "test") == expected,
    "replica selection falls back to the recomputed master"
  );
}

static void test_healthy_priority(void) {
  // Arrange
  configure_hosts();
  set_status(0, possible_master);
  set_status(1, healthy_master);
  set_status(2, healthy_master);
  save_master_index(0);

  // Act & Assert
  expect_master(1);

  // Arrange
  set_status(1, possible_master);

  // Act & Assert
  expect_master(2);

  // Arrange
  set_status(1, healthy_master);

  // Act & Assert
  expect_master(1);
}

static void test_possible_priority(void) {
  // Arrange
  configure_hosts();
  set_status(1, possible_master);
  set_status(2, possible_master);

  // Act & Assert
  expect_master(1);

  // Arrange
  save_master_index(2);

  // Act & Assert
  expect_master(1);
}

static void test_selected_master_becomes_replica(void) {
  // Arrange
  configure_hosts();
  set_status(0, possible_master);
  set_status(1, healthy_master);

  // Act & Assert
  expect_master(1);

  // Arrange
  set_status(1, replica);

  // Act & Assert
  expect_master(0);
}

static void test_selected_master_becomes_dead(void) {
  // Arrange
  configure_hosts();
  set_status(0, possible_master);
  set_status(1, healthy_master);

  // Act & Assert
  expect_master(1);

  // Arrange
  set_status(1, dead_host);

  // Act & Assert
  expect_master(0);

  // Arrange
  set_status(0, dead_host);

  // Act & Assert
  expect_master(-1);
}

static void test_no_master(void) {
  // Arrange
  configure_hosts();
  set_status(0, healthy_master);

  // Act & Assert
  expect_master(0);

  // Arrange
  set_status(0, replica);
  set_status(1, (MonitorStatus){.alive = true, .possible_dead = true});
  set_status(2, dead_host);

  // Act & Assert
  expect_master(-1);
}

static void test_dead_master_flags(void) {
  // Arrange
  configure_hosts();
  set_status(0, (MonitorStatus){.master = true});
  set_status(1, possible_master);
  set_status(2, (MonitorStatus){.master = true, .possible_dead = true});

  // Act & Assert
  expect_master(1);

  // Arrange
  set_status(1, replica);

  // Act & Assert
  expect_master(-1);
}

static void test_empty_hosts(void) {
  // Arrange
  configure_hosts();
  set_status(0, healthy_master);

  // Act & Assert
  expect_master(0);

  // Arrange
  host_count = 0;

  // Act & Assert
  expect_master(-1);
}

static void test_failover_marks_replica_on_other_timeline(void) {
  // Arrange: A is the master on timeline 1, and B, last seen on timeline 1,
  // has replayed all of its WAL.
  configure_hosts();
  MonitorHost *a = &monitor_host_list[0];
  MonitorHost *b = &monitor_host_list[1];
  MonitorHost *c = &monitor_host_list[2];
  a->has_master_wal = true;
  a->master_timeline = 1;
  set_status(0, healthy_master);
  b->replica_timeline = 1;
  b->replica_commit_age_ms = 60000;
  publish_monitor_snapshot(
    b, (MonitorSnapshot){.status = replica, .lag_ms = 300, .lsn = 0x9000}
  );
  expect_master(0);
  support_assert_true(
    !atomic_get_snapshot(b).status.timeline_mismatch,
    "B is on the master's timeline"
  );

  // Arrange: A fails, and C is promoted to timeline 2.
  set_status(0, dead_host);
  c->has_master_wal = true;
  c->master_timeline = 2;
  set_status(2, healthy_master);

  // Act & Assert
  expect_master(2);
  const MonitorSnapshot snap = atomic_get_snapshot(b);
  support_assert_true(
    snap.status.alive && snap.status.timeline_mismatch &&
      snap.lag_ms == 60000 && snap.lag_bytes == UINT64_MAX &&
      snap.lsn == 0x9000,
    "B is marked as soon as C is selected"
  );
  support_assert_true(
    b_at_selection.status.timeline_mismatch,
    "B is marked before the selection of C is published"
  );
}

static void test_failover_marks_possible_dead_replica(void) {
  // Arrange: B's last check failed, but its earlier one found it on
  // timeline 1, and it is still alive.
  configure_hosts();
  MonitorHost *a = &monitor_host_list[0];
  MonitorHost *b = &monitor_host_list[1];
  MonitorHost *c = &monitor_host_list[2];
  a->has_master_wal = true;
  a->master_timeline = 1;
  set_status(0, healthy_master);
  b->replica_timeline = 1;
  b->replica_commit_age_ms = 60000;
  publish_monitor_snapshot(
    b, (MonitorSnapshot){
         .status = {.alive = true, .possible_dead = true},
         .lag_ms = 300,
         .lsn = 0x9000,
         .timeline = 1,
       }
  );
  expect_master(0);

  // Arrange: A fails, and C is promoted to timeline 2.
  set_status(0, dead_host);
  c->has_master_wal = true;
  c->master_timeline = 2;
  set_status(2, healthy_master);

  // Act & Assert
  expect_master(2);
  const MonitorSnapshot snap = atomic_get_snapshot(b);
  support_assert_true(
    snap.status.alive && snap.status.possible_dead &&
      snap.status.timeline_mismatch && snap.lag_bytes == UINT64_MAX &&
      snap.timeline == 1,
    "a replica whose last check failed is marked as well"
  );
}

int main(const int argc, char **argv) {
  if (argc != 2) {
    support_fail("expected one test case name");
  }
  const struct {
    const char *name;
    support_action_t run;
  } cases[] = {
    {"healthy_priority", test_healthy_priority},
    {"possible_priority", test_possible_priority},
    {"selected_master_becomes_replica", test_selected_master_becomes_replica},
    {"selected_master_becomes_dead", test_selected_master_becomes_dead},
    {"no_master", test_no_master},
    {"dead_master_flags", test_dead_master_flags},
    {"empty_hosts", test_empty_hosts},
    {"failover_marks_replica_on_other_timeline",
     test_failover_marks_replica_on_other_timeline},
    {"failover_marks_possible_dead_replica",
     test_failover_marks_possible_dead_replica},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    if (strcmp(argv[1], cases[i].name) == 0) {
      cases[i].run();
      return EXIT_SUCCESS;
    }
  }
  support_fail("unknown test case name");
}
