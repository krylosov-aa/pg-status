/**
 * Public HTTP API of pg-status.
 */

#ifndef PG_STATUS_API_H
#define PG_STATUS_API_H

#include <stdint.h>

#include "http_server.h"

/**
 * Starts the pg-status HTTP API using the current in-memory monitor state.
 */
HTTPServer *start_pg_status_api(const char *listen_address, uint16_t port);

#endif  // PG_STATUS_API_H
