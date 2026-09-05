#ifndef PG_STATUS_CONNECTION_H
#define PG_STATUS_CONNECTION_H

#include "pg_monitor.h"

enum { PG_CONNECTION_PARAMETER_COUNT = 6 };

/**
 * Exact keyword/value arrays passed to libpq for one connection attempt.
 * The arrays are terminated by null pointers as required by libpq.
 */
typedef struct {
  const char *keywords[PG_CONNECTION_PARAMETER_COUNT + 1];
  const char *values[PG_CONNECTION_PARAMETER_COUNT + 1];
} PGConnectionParameters;

/**
 * Builds parameters without conninfo serialization, quoting, or escaping.
 * All values remain separate strings owned by the process configuration or
 * by the immutable MonitorHost.
 */
PGConnectionParameters pg_connection_parameters(const MonitorHost *host);

/**
 * Starts a libpq connection with separately supplied parameter values.
 * expand_dbname is disabled so a database name can never become conninfo.
 */
struct pg_conn *pg_connection_start(const MonitorHost *host);

#endif  // PG_STATUS_CONNECTION_H
