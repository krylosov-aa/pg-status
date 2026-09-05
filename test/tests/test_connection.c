/** Tests for safe libpq connection parameter handling. */

#include <libpq-fe.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common_support.h"
#include "connection.h"
#include "logger.h"
#include "pg_monitor.h"

static const char *find_connection_option(
  const PQconninfoOption *options, const char *keyword
) {
  for (const PQconninfoOption *option = options; option->keyword; option++) {
    if (strcmp(option->keyword, keyword) == 0) {
      return option->val;
    }
  }
  support_fail("libpq connection option not found");
}

static void test_separate_values(void) {
  // Arrange
  parameters.user = "user with spaces 'quotes' \\slashes Юникод";
  parameters.password = "password=not-a-key ' \\ 密码";
  parameters.database =
    "host=attacker.example password=injected db name ' \\ БД";
  parameters.connect_timeout = "17";
  const MonitorHost host = {.host = "2001:db8::1", .port = "6543"};
  const char *expected_keywords[] = {
    "user", "password", "host", "port", "dbname", "connect_timeout",
  };
  const char *expected_values[] = {
    parameters.user, parameters.password, host.host,
    host.port,       parameters.database, parameters.connect_timeout,
  };

  // Act
  const PGConnectionParameters actual = pg_connection_parameters(&host);

  // Assert
  for (size_t i = 0; i < PG_CONNECTION_PARAMETER_COUNT; i++) {
    support_assert_string_equal(
      actual.keywords[i], expected_keywords[i], "connection keyword"
    );
    support_assert_string_equal(
      actual.values[i], expected_values[i], "connection value"
    );
  }
  support_assert_true(
    actual.keywords[PG_CONNECTION_PARAMETER_COUNT] == nullptr,
    "connection keywords are not terminated"
  );
  support_assert_true(
    actual.values[PG_CONNECTION_PARAMETER_COUNT] == nullptr,
    "connection values are not terminated"
  );
}

static void test_dbname_not_expanded(void) {
  // Arrange
  parameters.user = "expected user";
  parameters.password = "expected password";
  parameters.database =
    "host=attacker.example user=attacker password=exposed dbname=other";
  parameters.connect_timeout = "1";
  const MonitorHost host = {.host = "127.0.0.1", .port = "1"};

  // Act
  PGconn *connection = pg_connection_start(&host);

  // Assert
  support_assert_true(connection != nullptr, "PQconnectStartParams failed");
  support_assert_string_equal(PQhost(connection), host.host, "libpq host");
  support_assert_string_equal(PQport(connection), host.port, "libpq port");
  support_assert_string_equal(
    PQuser(connection), parameters.user, "libpq user"
  );
  support_assert_string_equal(
    PQpass(connection), parameters.password, "libpq password"
  );
  support_assert_string_equal(
    PQdb(connection), parameters.database, "libpq dbname was expanded"
  );

  // Cleanup
  PQfinish(connection);
}

static void test_tls_environment(void) {
  // Arrange
  parameters.user = "postgres";
  parameters.password = "postgres";
  parameters.database = "postgres";
  parameters.connect_timeout = "1";
  const MonitorHost host = {.host = "database.example", .port = "5432"};
  const char *environment_names[] = {
    "PGSSLMODE", "PGSSLROOTCERT", "PGSSLCRL", "PGSSLCERT", "PGSSLKEY",
  };
  const char *environment_values[] = {
    "verify-full",     "/tls/root.crt",   "/tls/root.crl",
    "/tls/client.crt", "/tls/client.key",
  };
  const char *option_names[] = {
    "sslmode", "sslrootcert", "sslcrl", "sslcert", "sslkey",
  };
  for (size_t i = 0;
       i < sizeof(environment_names) / sizeof(environment_names[0]); i++) {
    support_set_environment(environment_names[i], environment_values[i]);
  }

  // Act
  PGconn *connection = pg_connection_start(&host);
  support_assert_true(connection != nullptr, "PQconnectStartParams failed");
  PQconninfoOption *options = PQconninfo(connection);
  support_assert_true(options != nullptr, "PQconninfo failed");

  // Assert
  const PGConnectionParameters connection_parameters = pg_connection_parameters(
    &host
  );
  for (size_t i = 0; i < PG_CONNECTION_PARAMETER_COUNT; i++) {
    support_assert_true(
      strncmp(connection_parameters.keywords[i], "ssl", 3) != 0,
      "TLS option was passed explicitly"
    );
  }
  for (size_t i = 0; i < sizeof(option_names) / sizeof(option_names[0]); i++) {
    support_assert_string_equal(
      find_connection_option(options, option_names[i]), environment_values[i],
      "libpq TLS environment option"
    );
  }

  // Cleanup
  PQconninfoFree(options);
  PQfinish(connection);
  for (size_t i = 0;
       i < sizeof(environment_names) / sizeof(environment_names[0]); i++) {
    support_clear_environment(environment_names[i]);
  }
}

static void run_monitor_with_secret(void) {
  pg_status_log_init();
  start_pg_monitor();
  stop_pg_monitor();
  pg_status_log_shutdown();
}

static void test_secret_marker_not_logged(void) {
  // Arrange
  static const char secret_marker[] =
    "PG_STATUS_PASSWORD_SECRET_MARKER_'_\\_Юникод";
  support_set_environment("pg_status__hosts", "127.0.0.1");
  support_set_environment("pg_status__pg_port", "1");
  support_set_environment("pg_status__pg_password", secret_marker);
  support_set_environment("pg_status__connect_timeout", "1");
  support_set_environment("pg_status__query_timeout_ms", "1000");
  support_set_environment("pg_status__sleep_ms", "1");
  support_set_environment("PGSSLMODE", "disable");

  // Act
  char *logs = support_capture_standard_error(run_monitor_with_secret);

  // Assert
  support_assert_contains(
    logs, "PostgreSQL operation failed", "missing connection diagnostic"
  );
  support_assert_contains(logs, "host=127.0.0.1", "missing safe host context");
  support_assert_contains(logs, "started hosts=1", "missing startup log");
  support_assert_not_contains(logs, secret_marker, "password leaked to logs");
  support_assert_not_contains(
    logs, "password=", "connection parameters leaked to logs"
  );

  // Cleanup
  free(logs);
}

static const struct {
  const char *name;
  support_action_t function;
} test_cases[] = {
  {"separate_values", test_separate_values},
  {"dbname_not_expanded", test_dbname_not_expanded},
  {"tls_environment", test_tls_environment},
  {"secret_marker_not_logged", test_secret_marker_not_logged},
};

int main(const int argc, char **argv) {
  if (argc != 2) {
    support_fail("expected one test case name");
  }

  for (size_t i = 0; i < sizeof(test_cases) / sizeof(test_cases[0]); i++) {
    if (strcmp(argv[1], test_cases[i].name) == 0) {
      test_cases[i].function();
      printf("connection_test %s passed\n", argv[1]);
      return EXIT_SUCCESS;
    }
  }

  support_fail("unknown test case name");
}
