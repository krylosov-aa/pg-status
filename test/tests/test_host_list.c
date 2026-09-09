/** Validate host/port lists without losing positional correspondence. */

#include <stdlib.h>
#include <string.h>

#include "common_support.h"
#include "pg_monitor.h"

static void test_valid_ports(const bool per_host) {
  const char *expected_hosts[] = {"A", "B", "C"};
  const char *expected_ports[] = {"5432", "6432", "7432"};
  parameters.hosts = "A,B,C";
  parameters.port = per_host ? "5432,6432,7432" : "5432";

  init_monitor_host_list();

  const size_t expected_count = sizeof(expected_hosts) /
                                sizeof(expected_hosts[0]);
  support_assert_true(host_count == expected_count, "unexpected host count");
  for (size_t i = 0; i < expected_count; i++) {
    support_assert_string_equal(
      monitor_host_list[i].host, expected_hosts[i], "host order changed"
    );
    support_assert_string_equal(
      monitor_host_list[i].port, expected_ports[per_host ? i : 0],
      "port assigned to the wrong host"
    );
  }
}

static const struct {
  const char *name;
  const char *hosts;
  const char *ports;
} invalid_cases[] = {
  {"empty_ports", "A,B,C", ""},
  {"leading_empty_port", "A,B,C", ",5432,6432"},
  {"middle_empty_port", "A,B,C", "5432,,6432"},
  {"trailing_empty_port", "A,B,C", "5432,6432,"},
  {"too_few_ports", "A,B,C", "5432,6432"},
  {"too_many_ports", "A,B,C", "5432,6432,7432,8432"},
  {"extra_port_single_host", "A", "5432,6432"},
  {"empty_hosts", "", "5432"},
  {"leading_empty_host", ",A,B", "5432,6432"},
  {"middle_empty_host", "A,,B", "5432,6432"},
  {"trailing_empty_host", "A,B,", "5432,6432"},
};

int main(const int argc, char **argv) {
  if (argc != 2) {
    support_fail("expected one test case name");
  }
  if (strcmp(argv[1], "shared_port") == 0) {
    test_valid_ports(false);
    return EXIT_SUCCESS;
  }
  if (strcmp(argv[1], "per_host_ports") == 0) {
    test_valid_ports(true);
    return EXIT_SUCCESS;
  }
  for (size_t i = 0; i < sizeof(invalid_cases) / sizeof(invalid_cases[0]);
       i++) {
    if (strcmp(argv[1], invalid_cases[i].name) == 0) {
      parameters.hosts = invalid_cases[i].hosts;
      parameters.port = invalid_cases[i].ports;
      init_monitor_host_list();
      support_fail("invalid host/port configuration was accepted");
    }
  }
  support_fail("unknown test case name");
}
