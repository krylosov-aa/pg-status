/**
 * Parameters and settings with which monitoring is started
 */

#include <ctype.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "logger.h"
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
  .hosts_dc = nullptr,
  .hosts_geo = nullptr,
  .current_dc = nullptr,
  .current_dc_env = nullptr,
  .dc_locality_configured = false,
  .dc_locality_enabled = false,
  .current_geo = nullptr,
  .current_geo_env = nullptr,
  .geo_locality_configured = false,
  .geo_locality_enabled = false,
  .port = "5432",
  .sleep_ms = 1000,
  .max_fails = 3,
  .sync_max_lag_ms = 1000,
  .sync_max_lag_bytes = 1000000,  // 1 mb
  .conn_max_age_ms = 300000,      // 5 minutes
  .query_timeout_ms = 1000,
};

static bool is_valid_environment_name(const char *name) {
  if (!name || !*name) {
    return false;
  }
  const unsigned char first = (unsigned char)*name;
  if (!(isalpha(first) || first == '_')) {
    return false;
  }
  for (const unsigned char *c = (const unsigned char *)name + 1; *c; c++) {
    if (!(isalnum(*c) || *c == '_')) {
      return false;
    }
  }
  return true;
}

static bool environment_has_value(const char *name) {
  const char *value = getenv(name);
  return value && *value;
}

static const char *resolve_locality(
  const char *direct_name, const char *source_name,
  const char *source_parameter_name
) {
  const char *direct = getenv(direct_name);
  if (direct && *direct) {
    return direct;
  }
  if (
    !is_valid_environment_name(source_name) ||
    strcmp(source_name, source_parameter_name) == 0
  ) {
    return nullptr;
  }
  const char *indirect = getenv(source_name);
  return indirect && *indirect ? indirect : nullptr;
}

static void set_locality(void) {
  parameters.dc_locality_configured =
    environment_has_value("pg_status__hosts_dc") ||
    environment_has_value("pg_status__current_dc") ||
    environment_has_value("pg_status__current_dc_env");
  parameters.geo_locality_configured =
    environment_has_value("pg_status__hosts_geo") ||
    environment_has_value("pg_status__current_geo") ||
    environment_has_value("pg_status__current_geo_env");

  replace_from_env("pg_status__hosts_dc", &parameters.hosts_dc);
  replace_from_env("pg_status__hosts_geo", &parameters.hosts_geo);
  replace_from_env("pg_status__current_dc_env", &parameters.current_dc_env);
  replace_from_env("pg_status__current_geo_env", &parameters.current_geo_env);

  parameters.current_dc = resolve_locality(
    "pg_status__current_dc", parameters.current_dc_env,
    "pg_status__current_dc_env"
  );
  parameters.current_geo = resolve_locality(
    "pg_status__current_geo", parameters.current_geo_env,
    "pg_status__current_geo_env"
  );
}

static void set_sleep(void) {
  const char *env_val = getenv("pg_status__sleep_ms");
  if (env_val && *env_val) {
    parameters.sleep_ms = str_to_int_greater_or_equal_zero(env_val);
  }

  env_val = getenv("pg_status__sleep");
  if (env_val && *env_val) {
    pg_status_log_fatal(
      "config",
      "pg_status__sleep is deprecated! Use pg_status__sleep_ms instead!"
    );
  }
}

static void set_hosts(void) {
  replace_from_env("pg_status__hosts", &parameters.hosts);
  if (!parameters.hosts || !*parameters.hosts) {
    pg_status_log_fatal("config", "pg_status__hosts is not set");
  }
}

/**
 * Overrides default parameters if they are set in environment variables.
 */
void set_parameters_from_env(void) {
  replace_from_env("pg_status__pg_user", &parameters.user);
  replace_from_env("pg_status__pg_database", &parameters.database);
  replace_from_env("pg_status__pg_password", &parameters.password);
  replace_from_env("pg_status__pg_port", &parameters.port);
  replace_from_env_uint("pg_status__max_fails", &parameters.max_fails);
  replace_from_env_ull(
    "pg_status__sync_max_lag_ms", &parameters.sync_max_lag_ms
  );
  replace_from_env_ull(
    "pg_status__sync_max_lag_bytes", &parameters.sync_max_lag_bytes
  );
  replace_from_env_ull(
    "pg_status__conn_max_age_ms", &parameters.conn_max_age_ms
  );
  replace_from_env_ull(
    "pg_status__query_timeout_ms", &parameters.query_timeout_ms
  );
  set_locality();
  set_sleep();
  if (parameters.sleep_ms == 0) {
    pg_status_log_fatal("config", "pg_status__sleep_ms must be greater than 0");
  }
  if (
    parameters.query_timeout_ms == 0 ||
    parameters.query_timeout_ms > (uint64_t)INT_MAX
  ) {
    pg_status_log_fatal(
      "config", "pg_status__query_timeout_ms must be between 1 and %d", INT_MAX
    );
  }
  set_hosts();
}
