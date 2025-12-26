#include <stdatomic.h>
#include <stdlib.h>

#include "pg_monitor.h"
#include "utils.h"

#ifdef __APPLE__
    #include <libpq-fe.h>
#else
    #include <postgresql/libpq-fe.h>
#endif
#include <stdio.h>


bool is_f(const char *pg_val) {
    return is_equal_strings(pg_val, "f");
}

bool is_t(const char *pg_val) {
    return is_equal_strings(pg_val, "t");
}


PGconn *db_connect(const char *connection_str) {
    PGconn *conn = PQconnectdb(connection_str);
    if (PQstatus(conn) != CONNECTION_OK) {
        printf_error(
            "\033[0;31m connect error: \033[0m %s \n ", PQerrorMessage(conn)
        );
        PQfinish(conn);
        return nullptr;
    }

    return conn;
}

int check_exec_result(const PGconn *conn, const PGresult *result) {
    const ExecStatusType resStatus = PQresultStatus(result);
    if (resStatus != PGRES_TUPLES_OK && resStatus != PGRES_COMMAND_OK) {
        printf_error(
            "\033[0;31m execute sql error: \033[0m %s \n ",
            PQerrorMessage(conn)
        );

        return 1;
    }
    return 0;
}

PGresult *execute_sql(PGconn *conn, const char *query) {
    PGresult *res = PQexec(conn, query);
    const int check_result = check_exec_result(conn, res);
    if (check_result == 1) {
        PQclear(res);
        return nullptr;
    }
    return res;
}

PGresult *connect_and_execute(const char *connection_str, const char *query) {
    PGconn *conn = db_connect(connection_str);
    if (!conn)
        return nullptr;

    return execute_sql(conn, query);
}

int extract_bool_value(PGresult *q_res, bool *result) {
    if (q_res == nullptr)
        return 1;

    if (PQntuples(q_res) > 0 && PQnfields(q_res) > 0)
        *result = is_t(PQgetvalue(q_res, 0, 0));
    else
        return 1;

    PQclear(q_res);
    return 0;
}


int execute_sql_bool(PGconn *conn, const char *query, bool *result) {
    PGresult *q_res = execute_sql(conn, query);
    return extract_bool_value(q_res, result);
}

const char *streaming_replication_query =
    "with is_in_recovery as (\n"
    "  select pg_is_in_recovery() is_replica\n"
    ")\n"
    "SELECT\n"
    "    is_replica\n"
    "  , case when not is_replica then pg_current_wal_lsn() end master_lsn\n"
    "  , case when is_replica then pg_last_wal_receive_lsn() end replica_received_lsn\n"
    "  , case when is_replica then pg_last_wal_replay_lsn() end replica_lsn\n"
    "  , case when is_replica\n"
    "      then coalesce((extract(epoch from now() - pg_last_xact_replay_timestamp()) * 1000)::bigint, 0)\n"
    "      else 0 end replica_delay_ms\n"
    "from is_in_recovery;\n";

unsigned long long parse_lsn(const char *lsn) {
    unsigned int hi, lo;
    if (!lsn || sscanf(lsn, "%X/%X", &hi, &lo) != 2)
        return 0;
    return (unsigned long long)hi << 32 | lo;
}

unsigned long long max_lsn(unsigned long long  a, unsigned long long  b) {
    return a > b ? a : b;
}

void check_host_streaming_replication(
    MonitorHost *host, const unsigned int max_fails
) {
    static unsigned long long master_lsn = 0;
    MonitorStatus *status = atomic_get_status(host);
    MonitorStatus *new_status = atomic_load_explicit(
        &host -> not_actual_status, memory_order_acquire
    );

    PGresult *q_res = connect_and_execute(
        host -> connection_str, streaming_replication_query
    );

    if (!q_res) {
        printf("%s: dead\n", host -> host);
        host -> failed_connections++;
        if (host -> failed_connections > max_fails) {
            new_status -> alive = false;
            new_status -> is_master = false;
        }
    }
    else {
        new_status -> alive = true;
        host -> failed_connections = 0;

        const bool is_replica = is_t(PQgetvalue(q_res, 0, 0));
        if (is_replica) {
            printf("%s: replica\n", host -> host);
            new_status -> is_master = false;
            new_status -> delay_ms = str_to_ull(PQgetvalue(q_res, 0, 4));

            unsigned long long replica_received_lsn = parse_lsn(PQgetvalue(q_res, 0, 2));
            unsigned long long replica_lsn = parse_lsn(PQgetvalue(q_res, 0, 3));
            new_status -> delay_bytes = max_lsn(master_lsn, replica_received_lsn) - replica_lsn;
        }
        else {
            printf("%s: master\n", host -> host);
            new_status -> is_master = true;
            new_status -> delay_ms = 0;
            new_status -> delay_bytes = 0;
            master_lsn = parse_lsn(PQgetvalue(q_res, 0, 1));
        }
    }

    atomic_store_explicit(&host -> status, new_status, memory_order_release);
    atomic_store_explicit(&host -> not_actual_status, status, memory_order_release);
}
