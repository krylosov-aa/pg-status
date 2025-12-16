#include "pg_monitor.h"
#include "utils.h"

#include <libpq-fe.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>


bool is_f(const char *pg_val) {
    return is_equal_strings(pg_val, "f");
}

bool is_t(const char *pg_val) {
    return is_equal_strings(pg_val, "t");
}


PGconn *db_connect(const char *connection_str) {
    // printf("connection_str %s\n", connection_str);
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

char default_hosts_str[] = "localhost,127.0.0.1";

MonitorParameters parameters = {
    .user = "postgres",
    .password = "postgres",
    .database = "postgres",
    .hosts_delimiter = ",",
    .hosts = default_hosts_str,
    .port = "6432",
    .connect_timeout = "2",
    .sleep = 5,
};

void replace_from_env(const char *env_name, char **result) {
    char *env_val = getenv(env_name);
    if (env_val && *env_val)
        *result = env_val;
}

void replace_from_env_copy(const char *env_name, char **result) {
    const char *env_val = getenv(env_name);
    if (env_val != nullptr && *env_val) {
        char *env_val_copy = copy_string(env_val);
        *result = env_val_copy;
    }
}

void get_values_from_env(void) {
    replace_from_env("pg_status__pg_user", &parameters.user);
    replace_from_env("pg_status__pg_database", &parameters.database);
    replace_from_env("pg_status__pg_password", &parameters.password);
    replace_from_env("pg_status__delimiter", &parameters.hosts_delimiter);
    replace_from_env("pg_status__port", &parameters.port);
    replace_from_env("pg_status__connect_timeout", &parameters.connect_timeout);

    replace_from_env_copy("pg_status__hosts", &parameters.hosts);

    const char *env_val = getenv("pg_status__sleep");
    if (env_val != nullptr && *env_val) {
        parameters.sleep = str_to_uint(env_val);
    }
}

ConnectionStrings get_connection_strings(void) {
    get_values_from_env();

    char *save_hosts = nullptr;
    char *hosts = copy_string(parameters.hosts);
    char *host = strtok_r(hosts, parameters.hosts_delimiter, &save_hosts);

    ConnectionStrings result = {
        .cnt = 0
    };


    while (host != nullptr && result.cnt < MAX_HOSTS) {
        char *connection_str = format_string(
            "user=%s password=%s host=%s port=%s "
            "dbname=%s connect_timeout=%s",
            parameters.user, parameters.password, host, parameters.port,
            parameters.database, parameters.connect_timeout
        );
        result.connection_str[result.cnt] = connection_str;
        result.hosts[result.cnt] = copy_string(host);
        result.cnt = result.cnt + 1;
        host = strtok_r(nullptr, parameters.hosts_delimiter, &save_hosts);
    }
    if (host != nullptr && result.cnt == MAX_HOSTS)
        raise_error("Too many hosts. Maximum value = %d", MAX_HOSTS);

    free(hosts);

    return result;
}


_Atomic(MonitorStatus *) monitor_status = nullptr;

MonitorStatus buffers[2];

void init_buffers(const unsigned int hosts_cnt) {
    for (int i = 0; i < 2; i++) {
        buffers[i].master = "null";
        buffers[i].replicas = calloc(hosts_cnt, sizeof(char *));
        buffers[i].liveness = calloc(hosts_cnt, sizeof(HostLiveness));
        buffers[i].cnt = hosts_cnt;
    }

    atomic_store_explicit(&monitor_status, &buffers[0], memory_order_release);
}

void reset_inactive(MonitorStatus *inactive) {
    inactive -> master = "null";
    memset(
        inactive -> replicas,
        0,
        inactive -> cnt * sizeof(char *)
    );
    memset(
        inactive -> liveness,
        0,
        inactive -> cnt * sizeof(HostLiveness)
    );
}

MonitorStatus *prep_inactive(void) {
    const MonitorStatus *active = atomic_load_explicit(
        &monitor_status, memory_order_acquire
    );
    MonitorStatus *inactive = active == &buffers[0] ? &buffers[1] : &buffers[0];
    reset_inactive(inactive);
    return inactive;
}

void check_hosts(const ConnectionStrings con_str_list) {
    MonitorStatus *inactive = prep_inactive();

    char **replicas_cursor = inactive -> replicas;
    HostLiveness *liveness_cursor = inactive -> liveness;

    for (uint i = 0; i < con_str_list.cnt; i++) {
        bool is_replica = true;
        char *host = con_str_list.hosts[i];
        const int exit_val = is_host_in_recovery(
            con_str_list.connection_str[i],
            &is_replica
        );

        if (exit_val == 0) {
            liveness_cursor -> host = host;
            liveness_cursor -> alive = true;
            liveness_cursor++;

            if (is_replica) {
                printf("%s: replica\n", host);
                *replicas_cursor = host;
                replicas_cursor++;
            }
            else {
                printf("%s: master\n", host);
                inactive -> master = host;
            }
        }
        else {
            printf("%s: dead\n", host);
            liveness_cursor -> host = host;
            liveness_cursor -> alive = false;
            liveness_cursor++;
        }
    }
    printf("\n");

    atomic_store_explicit(&monitor_status, inactive, memory_order_release);
}

const MonitorStatus *get_cur_stat(void) {
    return atomic_load_explicit(
        &monitor_status, memory_order_acquire
    );
}

atomic_uint pg_monitor_running = 1;

void stop_pg_monitor(void) {
    atomic_store(&pg_monitor_running, true);
    printf("pg_monitor stopped\n");
}

void *pg_monitor_thread(void *arg) {
    (void)arg;

    const ConnectionStrings con_str_list = get_connection_strings();
    init_buffers(con_str_list.cnt);

    while (atomic_load(&pg_monitor_running)) {
        check_hosts(con_str_list);
        sleep(parameters.sleep);
    }
    return nullptr;
}

pthread_t start_pg_monitor() {
    pthread_t pg_monitor_tid;
    const int started = pthread_create(
        &pg_monitor_tid, nullptr, pg_monitor_thread, nullptr
    );

    if (started != 0)
        raise_error("Failed to start pg_monitor");

    printf("pg_monitor started\n");
    return pg_monitor_tid;
}
