/** Tests for env-only DC/geo configuration and positional host metadata. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common_support.h"
#include "logger.h"
#include "pg_monitor.h"

static void set_base_hosts(void) {
  const char *locality_environment[] = {
    "pg_status__current_dc",
    "pg_status__current_dc_env",
    "pg_status__hosts_dc",
    "pg_status__current_geo",
    "pg_status__current_geo_env",
    "pg_status__hosts_geo",
    "IGNORED_DC",
    "TEST_CURRENT_GEO",
    "MISSING_CURRENT_DC",
  };
  for (size_t i = 0;
       i < sizeof(locality_environment) / sizeof(locality_environment[0]);
       i++) {
    support_clear_environment(locality_environment[i]);
  }
  support_set_environment(
    "pg_status__hosts",
    "db-frankfurt.example,db-amsterdam.example,db-virginia.example"
  );
  support_set_environment("pg_status__pg_port", "5432");
}

static void load_locality(void) {
  pg_status_log_init();
  set_parameters_from_env();
  init_monitor_host_list();
  pg_status_log_shutdown();
}

static void assert_locality(
  const unsigned int index, const char *dc, const char *geo
) {
  support_assert_string_equal(
    monitor_host_list[index].dc, dc, "unexpected host DC"
  );
  support_assert_string_equal(
    monitor_host_list[index].geo, geo, "unexpected host geo"
  );
}

static void test_complete_direct_and_indirect(void) {
  // Arrange
  set_base_hosts();
  support_set_environment("pg_status__current_dc", "frankfurt");
  support_set_environment("pg_status__current_dc_env", "IGNORED_DC");
  support_set_environment("IGNORED_DC", "amsterdam");
  support_set_environment(
    "pg_status__hosts_dc", "frankfurt,amsterdam,virginia"
  );
  support_set_environment("pg_status__current_geo_env", "TEST_CURRENT_GEO");
  support_set_environment("TEST_CURRENT_GEO", "europe");
  support_set_environment(
    "pg_status__hosts_geo", "europe,europe,north-america"
  );

  // Act
  char *logs = support_capture_standard_error(load_locality);

  // Assert
  support_assert_string_equal(parameters.current_dc, "frankfurt", "direct DC");
  support_assert_string_equal(parameters.current_geo, "europe", "indirect geo");
  support_assert_true(parameters.dc_locality_enabled, "DC not enabled");
  support_assert_true(parameters.geo_locality_enabled, "geo not enabled");
  assert_locality(0, "frankfurt", "europe");
  assert_locality(1, "amsterdam", "europe");
  assert_locality(2, "virginia", "north-america");
  support_assert_not_contains(
    logs, "preference disabled", "complete locality emitted warning"
  );

  // Cleanup
  free(logs);
}

static void test_no_locality_is_silent(void) {
  // Arrange
  set_base_hosts();

  // Act
  char *logs = support_capture_standard_error(load_locality);

  // Assert
  support_assert_true(parameters.current_dc == nullptr, "unexpected DC");
  support_assert_true(parameters.current_geo == nullptr, "unexpected geo");
  support_assert_true(!parameters.dc_locality_enabled, "unexpected enabled DC");
  support_assert_true(
    !parameters.geo_locality_enabled, "unexpected enabled geo"
  );
  for (unsigned int i = 0; i < host_count; i++) {
    support_assert_true(
      monitor_host_list[i].dc == nullptr, "unexpected host DC"
    );
    support_assert_true(
      monitor_host_list[i].geo == nullptr, "unexpected host geo"
    );
  }
  support_assert_not_contains(
    logs, "preference disabled", "unset locality emitted warning"
  );

  // Cleanup
  free(logs);
}

static void test_empty_locality_is_silent(void) {
  // Arrange
  set_base_hosts();
  support_set_environment("pg_status__current_dc", "");
  support_set_environment("pg_status__current_dc_env", "");
  support_set_environment("pg_status__hosts_dc", "");
  support_set_environment("pg_status__current_geo", "");
  support_set_environment("pg_status__current_geo_env", "");
  support_set_environment("pg_status__hosts_geo", "");

  // Act
  char *logs = support_capture_standard_error(load_locality);

  // Assert
  support_assert_true(parameters.current_dc == nullptr, "unexpected DC");
  support_assert_true(parameters.current_geo == nullptr, "unexpected geo");
  support_assert_true(!parameters.dc_locality_enabled, "unexpected enabled DC");
  support_assert_true(
    !parameters.geo_locality_enabled, "unexpected enabled geo"
  );
  support_assert_not_contains(
    logs, "preference disabled", "empty locality emitted warning"
  );

  // Cleanup
  free(logs);
}

static void test_empty_direct_uses_indirect(void) {
  // Arrange
  set_base_hosts();
  support_set_environment("pg_status__current_dc", "");
  support_set_environment("pg_status__current_dc_env", "IGNORED_DC");
  support_set_environment("IGNORED_DC", "amsterdam");
  support_set_environment(
    "pg_status__hosts_dc", "frankfurt,amsterdam,virginia"
  );

  // Act
  char *logs = support_capture_standard_error(load_locality);

  // Assert
  support_assert_string_equal(
    parameters.current_dc, "amsterdam", "empty direct DC did not fall back"
  );
  support_assert_true(parameters.dc_locality_enabled, "DC not enabled");
  support_assert_not_contains(
    logs, "DC preference disabled", "valid indirect DC emitted warning"
  );

  // Cleanup
  free(logs);
}

static void test_partial_dimensions_warn_independently(void) {
  // Arrange
  set_base_hosts();
  support_set_environment("pg_status__current_dc", "frankfurt");
  support_set_environment(
    "pg_status__hosts_geo", "europe,europe,north-america"
  );

  // Act
  char *logs = support_capture_standard_error(load_locality);

  // Assert
  support_assert_contains(
    logs, "DC preference disabled", "missing partial DC warning"
  );
  support_assert_contains(
    logs, "geo preference disabled", "missing partial geo warning"
  );
  support_assert_true(monitor_host_list[0].dc == nullptr, "unexpected host DC");
  support_assert_true(!parameters.dc_locality_enabled, "partial DC enabled");
  support_assert_true(!parameters.geo_locality_enabled, "partial geo enabled");
  support_assert_string_equal(
    monitor_host_list[0].geo, "europe", "valid host geo metadata"
  );

  // Cleanup
  free(logs);
}

static void test_empty_position_disables_only_dc(void) {
  // Arrange
  set_base_hosts();
  support_set_environment("pg_status__current_dc", "frankfurt");
  support_set_environment("pg_status__hosts_dc", "frankfurt,,virginia");
  support_set_environment("pg_status__current_geo", "europe");
  support_set_environment(
    "pg_status__hosts_geo", "europe,europe,north-america"
  );

  // Act
  char *logs = support_capture_standard_error(load_locality);

  // Assert
  support_assert_contains(
    logs, "DC preference disabled", "missing empty-position warning"
  );
  support_assert_not_contains(
    logs, "geo preference disabled", "valid geo was disabled"
  );
  support_assert_true(!parameters.dc_locality_enabled, "partial DC enabled");
  support_assert_true(parameters.geo_locality_enabled, "complete geo disabled");
  for (unsigned int i = 0; i < host_count; i++) {
    support_assert_true(
      monitor_host_list[i].dc == nullptr, "partial DC applied"
    );
  }
  support_assert_string_equal(
    monitor_host_list[1].geo, "europe", "valid geo mapping"
  );

  // Cleanup
  free(logs);
}

static void test_wrong_cardinality_disables_dc(void) {
  // Arrange
  set_base_hosts();
  support_set_environment("pg_status__current_dc", "frankfurt");
  support_set_environment("pg_status__hosts_dc", "frankfurt,amsterdam");

  // Act
  char *logs = support_capture_standard_error(load_locality);

  // Assert
  support_assert_contains(
    logs, "DC preference disabled", "missing cardinality warning"
  );
  for (unsigned int i = 0; i < host_count; i++) {
    support_assert_true(
      monitor_host_list[i].dc == nullptr, "partial DC applied"
    );
  }

  // Cleanup
  free(logs);
}

static void test_extra_value_disables_geo(void) {
  // Arrange
  set_base_hosts();
  support_set_environment("pg_status__current_geo", "europe");
  support_set_environment(
    "pg_status__hosts_geo", "europe,europe,north-america,asia"
  );

  // Act
  char *logs = support_capture_standard_error(load_locality);

  // Assert
  support_assert_contains(
    logs, "geo preference disabled", "missing extra-value warning"
  );
  for (unsigned int i = 0; i < host_count; i++) {
    support_assert_true(
      monitor_host_list[i].geo == nullptr, "invalid geo applied"
    );
  }

  // Cleanup
  free(logs);
}

static void test_unresolved_source_warns(void) {
  // Arrange
  set_base_hosts();
  support_set_environment("pg_status__current_dc_env", "MISSING_CURRENT_DC");
  support_set_environment(
    "pg_status__hosts_dc", "frankfurt,amsterdam,virginia"
  );

  // Act
  char *logs = support_capture_standard_error(load_locality);

  // Assert
  support_assert_true(
    parameters.current_dc == nullptr, "missing source resolved"
  );
  support_assert_contains(
    logs, "DC preference disabled", "missing unresolved-source warning"
  );

  // Cleanup
  free(logs);
}

static void test_invalid_and_self_referencing_sources_warn(void) {
  // Arrange
  set_base_hosts();
  support_set_environment("pg_status__current_dc_env", "INVALID=NAME");
  support_set_environment(
    "pg_status__current_geo_env", "pg_status__current_geo_env"
  );
  support_set_environment(
    "pg_status__hosts_dc", "frankfurt,amsterdam,virginia"
  );
  support_set_environment(
    "pg_status__hosts_geo", "europe,europe,north-america"
  );

  // Act
  char *logs = support_capture_standard_error(load_locality);

  // Assert
  support_assert_true(parameters.current_dc == nullptr, "invalid DC source");
  support_assert_true(
    parameters.current_geo == nullptr, "self-referencing geo source"
  );
  support_assert_contains(
    logs, "DC preference disabled", "missing invalid-source warning"
  );
  support_assert_contains(
    logs, "geo preference disabled", "missing self-reference warning"
  );

  // Cleanup
  free(logs);
}

static const struct {
  const char *name;
  support_action_t function;
} test_cases[] = {
  {"complete_direct_and_indirect", test_complete_direct_and_indirect},
  {"no_locality_is_silent", test_no_locality_is_silent},
  {"empty_locality_is_silent", test_empty_locality_is_silent},
  {"empty_direct_uses_indirect", test_empty_direct_uses_indirect},
  {"partial_dimensions_warn_independently",
   test_partial_dimensions_warn_independently},
  {"empty_position_disables_only_dc", test_empty_position_disables_only_dc},
  {"wrong_cardinality_disables_dc", test_wrong_cardinality_disables_dc},
  {"extra_value_disables_geo", test_extra_value_disables_geo},
  {"unresolved_source_warns", test_unresolved_source_warns},
  {"invalid_and_self_referencing_sources_warn",
   test_invalid_and_self_referencing_sources_warn},
};

int main(const int argc, char **argv) {
  if (argc != 2) {
    support_fail("expected one test case name");
  }
  for (size_t i = 0; i < sizeof(test_cases) / sizeof(test_cases[0]); i++) {
    if (strcmp(argv[1], test_cases[i].name) == 0) {
      test_cases[i].function();
      printf("locality_test %s passed\n", argv[1]);
      return EXIT_SUCCESS;
    }
  }
  support_fail("unknown test case name");
}
