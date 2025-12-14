#include "pg_monitor.h"
#include "utils.h"

#include <libpq-fe.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>


bool is_f(const char *pg_val) {
    return ps_is_equal_strings(pg_val, "f");
}

bool is_t(const char *pg_val) {
    return ps_is_equal_strings(pg_val, "t");
}


PGconn *db_connect(const char *connection_str) {
    // printf("connection_str %s\n", connection_str);
    PGconn *conn = PQconnectdb(connection_str);
    if (PQstatus(conn) != CONNECTION_OK) {
        ps_printf_error("\033[0;31m connect error: \033[0m %s \n ",PQerrorMessage(conn));
        PQfinish(conn);
        return nullptr;
    }

    return conn;
}

int check_exec_result(const PGconn *conn, const PGresult *result) {
    const ExecStatusType resStatus = PQresultStatus(result);
    int exit_value = 0;
    if (resStatus != PGRES_TUPLES_OK && resStatus != PGRES_COMMAND_OK) {
        ps_printf_error(
            "\033[0;31m execute sql error: \033[0m %s \n ",
            PQerrorMessage(conn)
        );

        exit_value = 1;
    }
    return exit_value;
}

PGresult *execute_sql(PGconn *conn, const char *query) {
    PGresult *res = PQexec(conn, query);
    const int check_result = check_exec_result(conn, res);
    if (check_result == 1) {
        PQclear(res);
        res = nullptr;
    }
    return res;
}


int extract_bool_value(PGresult *q_res, bool *result) {
    if (q_res == nullptr)
        return 1;

    int exit_val = 0;

    if (PQntuples(q_res) > 0 && PQnfields(q_res) > 0) {
        *result = is_t(PQgetvalue(q_res, 0, 0));
    }
    else
        exit_val = 1;

    PQclear(q_res);
    return exit_val;
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

int running = 1;

char default_hosts_str[] = "localhost,127.0.0.1";
char default_ports_str[] = "6432,6432";

MonitorParameters parameters = {
    .user = "postgres",
    .password = "postgres",
    .database = "postgres",
    .hosts_delimiter = ",",
    .hosts = default_hosts_str,
    .ports = default_ports_str,
    .connect_timeout = "2",
};

void replace_from_env(const char *env_name, char **result) {
    char *env_val = getenv(env_name);
    if (env_val && *env_val) {
        *result = env_val;
    }
}

void replace_from_env_copy(const char *env_name, char **result) {
    char *env_val = getenv(env_name);
    if (env_val && *env_val) {
        char *env_val_copy = ps_copy_string(env_val);
        *result = env_val_copy;
    }
}

void get_values_from_env(void) {
    replace_from_env("pg_status__pg_user", &parameters.user);
    replace_from_env("pg_status__pg_database", &parameters.database);
    replace_from_env("pg_status__pg_password", &parameters.password);
    replace_from_env("pg_status__delimiter", &parameters.hosts_delimiter);
    replace_from_env_copy("pg_status__hosts", &parameters.hosts);
    replace_from_env("pg_status__ports", &parameters.ports);
    replace_from_env("pg_status__connect_timeout", &parameters.connect_timeout);
}

ConnectionStrings get_connection_strings(void) {
    get_values_from_env();

    char *save_hosts = nullptr;
    char *save_ports = nullptr;
    char *hosts = ps_copy_string(parameters.hosts);
    char *ports = ps_copy_string(parameters.ports);
    char *host = strtok_r(hosts, parameters.hosts_delimiter, &save_hosts);
    char *port = strtok_r(ports, parameters.hosts_delimiter, &save_ports);
    ConnectionStrings result = {
        .cnt = 0
    };


    while (
          host != nullptr
        && port != nullptr
        && result.cnt < MAX_HOSTS
    ) {
        char *connection_str = ps_format_string(
            "user=%s password=%s host=%s port=%s "
            "dbname=%s connect_timeout=%s",
            parameters.user, parameters.password, host, port,
            parameters.database, parameters.connect_timeout
        );
        result.connection_str[result.cnt] = connection_str;
        result.hosts[result.cnt] = ps_copy_string(host);
        result.cnt = result.cnt + 1;
        host = strtok_r(nullptr, parameters.hosts_delimiter, &save_hosts);
        port = strtok_r(nullptr, parameters.hosts_delimiter, &save_ports);
    }
    free(hosts);
    free(ports);

    return result;
}


_Atomic(MonitorStatus *) status = nullptr;

MonitorStatus buffers[2];

void init_buffers(const unsigned int replicas_count) {
    for (int i = 0; i < 2; i++) {
        buffers[i].master = nullptr;
        buffers[i].replicas_cnt = replicas_count;
        buffers[i].replicas = malloc(
            replicas_count * sizeof(ReplicaStatus)
        );
    }

    atomic_thread_fence(memory_order_release);
    atomic_store(&status, &buffers[0]);
}

void reset_inactive(MonitorStatus *inactive) {
    inactive -> master = nullptr;
    memset(inactive->replicas, 0, inactive->replicas_cnt * sizeof(ReplicaStatus));
}

void check_hosts(const ConnectionStrings con_str_list) {
    MonitorStatus *active = atomic_load(&status);
    MonitorStatus *inactive = active == &buffers[0] ? &buffers[1] : &buffers[0];
    reset_inactive(inactive);

    ReplicaStatus *replicas_cursor = inactive -> replicas;

    for (uint i = 0; i < con_str_list.cnt; i++) {
        bool is_replica = true;
        const int exit_val = is_host_in_recovery(
            con_str_list.connection_str[i],
            &is_replica
        );

        if (exit_val == 0) {
            if (is_replica) {
                replicas_cursor -> host = con_str_list.hosts[i];
                replicas_cursor++;
                // printf("%s: replica\n", con_str_list.hosts[i]);
            }
            else {
                inactive -> master = con_str_list.hosts[i];
                // printf("%s: master\n", con_str_list.hosts[i]);
            }
        }
        else {
            printf("%s: dead\n", con_str_list.hosts[i]);
        }
    }

    atomic_thread_fence(memory_order_release);
    atomic_store(&status, inactive);
}

void *monitor_thread(
    // void *arg
) {
    // (void)arg;

    const ConnectionStrings con_str_list = get_connection_strings();
    init_buffers(con_str_list.cnt - 1);

    while (running) {
        check_hosts(con_str_list);

        MonitorStatus *cur_stat = atomic_load(&status);
        printf("master: %s\n", cur_stat -> master);
        for (uint i = 0; i < cur_stat->replicas_cnt; i++) {
            printf("replica: %s\n", cur_stat -> replicas[i].host);
        }
        printf("\n");
        sleep(1);
    }
    return nullptr;
}
