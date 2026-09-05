/** Safe construction of PostgreSQL connection parameters. */

#include "connection.h"

#include <libpq-fe.h>

PGConnectionParameters pg_connection_parameters(const MonitorHost *host) {
  return (PGConnectionParameters){
    .keywords =
      {
        "user",
        "password",
        "host",
        "port",
        "dbname",
        "connect_timeout",
        nullptr,
      },
    .values = {
      parameters.user,
      parameters.password,
      host->host,
      host->port,
      parameters.database,
      parameters.connect_timeout,
      nullptr,
    },
  };
}

struct pg_conn *pg_connection_start(const MonitorHost *host) {
  const PGConnectionParameters connection_parameters = pg_connection_parameters(
    host
  );
  return PQconnectStartParams(
    connection_parameters.keywords, connection_parameters.values, 0
  );
}
