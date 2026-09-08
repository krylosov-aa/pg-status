/** Master selection from published host states, including role changes. */

#include <stdlib.h>
#include <string.h>

#include "common_support.h"

// Exercise the actual private recomputation without exposing a test-only API.
#include "../../src/pg_monitor/pg_monitor.c"

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
  recompute_master_index();
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
  configure_hosts();
  set_status(0, possible_master);
  set_status(1, healthy_master);
  set_status(2, healthy_master);
  save_master_index(0);
  expect_master(1);
  set_status(1, possible_master);
  expect_master(2);
  set_status(1, healthy_master);
  expect_master(1);
}

static void test_possible_priority(void) {
  configure_hosts();
  set_status(1, possible_master);
  set_status(2, possible_master);
  expect_master(1);
  save_master_index(2);
  expect_master(1);
}

static void test_selected_master_becomes_replica(void) {
  configure_hosts();
  set_status(0, possible_master);
  set_status(1, healthy_master);
  expect_master(1);
  set_status(1, replica);
  expect_master(0);
}

static void test_selected_master_becomes_dead(void) {
  configure_hosts();
  set_status(0, possible_master);
  set_status(1, healthy_master);
  expect_master(1);
  set_status(1, dead_host);
  expect_master(0);
  set_status(0, dead_host);
  expect_master(-1);
}

static void test_no_master(void) {
  configure_hosts();
  set_status(0, healthy_master);
  expect_master(0);
  set_status(0, replica);
  set_status(1, (MonitorStatus){.alive = true, .possible_dead = true});
  set_status(2, dead_host);
  expect_master(-1);
}

static void test_dead_master_flags(void) {
  configure_hosts();
  set_status(0, (MonitorStatus){.master = true});
  set_status(1, possible_master);
  set_status(2, (MonitorStatus){.master = true, .possible_dead = true});
  expect_master(1);
  set_status(1, replica);
  expect_master(-1);
}

static void test_empty_hosts(void) {
  configure_hosts();
  set_status(0, healthy_master);
  expect_master(0);
  host_count = 0;
  expect_master(-1);
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
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    if (strcmp(argv[1], cases[i].name) == 0) {
      cases[i].run();
      return EXIT_SUCCESS;
    }
  }
  support_fail("unknown test case name");
}
