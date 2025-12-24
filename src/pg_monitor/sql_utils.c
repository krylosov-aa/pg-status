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


const char *in_recovery_query = "SELECT pg_is_in_recovery();";

int is_host_in_recovery(const char* connection_str, bool* result) {
    PGconn *conn = db_connect(connection_str);
    if (conn == nullptr)
        return 1;

    const int exit_val = execute_sql_bool(conn, in_recovery_query, result);
    PQfinish(conn);
    return exit_val;
}
