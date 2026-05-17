/**
 * Parameters and settings with which monitoring is started
 */

#include <limits.h>
#include <stdlib.h>

#include "pg_monitor.h"
#include "utils.h"

/**
 * pg-monitor parameters. The default parameters are set here.
 */
MonitorParameters parameters = {
  .user = "postgres",
  .password = "postgres",
  .database = "postgres",
  .hosts = nullptr,
  .port = "5432",
  .connect_timeout = "2",
  .sleep_ms = 5000,
  .max_fails = 3,
  .sync_max_lag_ms = 1000,
  .sync_max_lag_bytes = 1000000,  // 1 mb
};

static void set_sleep(void) {
  const char *env_val = getenv("pg_status__sleep_ms");
  if (env_val && *env_val) {
    parameters.sleep_ms = str_to_int_greater_or_equal_zero(env_val);
  }

  env_val = getenv("pg_status__sleep");
  if (env_val && *env_val) {
    raise_error(
      "pg_status__sleep is deprecated! Use pg_status__sleep_ms instead!"
    );
  }
}

static void set_hosts(void) {
  replace_from_env("pg_status__hosts", &parameters.hosts);
  if (!parameters.hosts || !*parameters.hosts) {
    raise_error("pg_status__hosts not set");
  }
}

/**
 * Overrides default parameters if they are set in environment variables.
 */
void set_parameters_from_env(void) {
  replace_from_env("pg_status__pg_user", &parameters.user);
  replace_from_env("pg_status__pg_database", &parameters.database);
  replace_from_env("pg_status__pg_password", &parameters.password);
  replace_from_env("pg_status__connect_timeout", &parameters.connect_timeout);
  replace_from_env("pg_status__pg_port", &parameters.port);
  replace_from_env_uint("pg_status__max_fails", &parameters.max_fails);
  replace_from_env_ull(
    "pg_status__sync_max_lag_ms", &parameters.sync_max_lag_ms
  );
  replace_from_env_ull(
    "pg_status__sync_max_lag_bytes", &parameters.sync_max_lag_bytes
  );
  set_sleep();
  set_hosts();
}
