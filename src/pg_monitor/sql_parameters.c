#include "pg_monitor.h"
#include "utils.h"
#include <stdlib.h>
#include <string.h>


MonitorParameters parameters = {
    .user = "postgres",
    .password = "postgres",
    .database = "postgres",
    .hosts_delimiter = ",",
    .hosts = nullptr,
    .port = "5432",
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
    replace_from_env("pg_status__connect_timeout", &parameters.connect_timeout);

    replace_from_env("pg_status__hosts", &parameters.hosts);
    if (parameters.hosts == nullptr)
        raise_error("pg_status__hosts not set");

    replace_from_env("pg_status__port", &parameters.port);

    const char *env_val = getenv("pg_status__sleep");
    if (env_val != nullptr && *env_val) {
        parameters.sleep = str_to_uint(env_val);
    }
}

char *next_host(char *hosts) {
    static char *save_ptr = nullptr;
    static char *host = nullptr;

    if (host == nullptr)
        host = strtok_r(hosts, parameters.hosts_delimiter, &save_ptr);
    else
        host = strtok_r(nullptr, parameters.hosts_delimiter, &save_ptr);
    return host;
}

/**
 * Returns ports sequentially. When no more ports are available,
 * it continues returning the last one.
 * This allows a single port to be used for all hosts.
 */
char *next_port(char *ports) {
    static char *last_port = nullptr;
    static char *save_ptr = nullptr;
    static char *port = nullptr;

    if (port == nullptr)
        port = strtok_r(ports, parameters.hosts_delimiter, &save_ptr);
    else
        port = strtok_r(nullptr, parameters.hosts_delimiter, &save_ptr);

    if (port != nullptr)
        last_port = port;
    return last_port;
}

void add_connection_string(
    ConnectionStrings *result,
    char *host,
    char *port
) {
    const unsigned int i = result -> cnt;
    char *connection_str = format_string(
        "user=%s password=%s host=%s port=%s "
        "dbname=%s connect_timeout=%s",
        parameters.user, parameters.password, host, port,
        parameters.database, parameters.connect_timeout
    );
    result -> connection_str[i] = connection_str;
    result -> hosts[i] = copy_string(host);
    result -> cnt = i + 1;
}

ConnectionStrings get_connection_strings(void) {
    get_values_from_env();

    char *hosts = copy_string(parameters.hosts);
    char *host = next_host(hosts);

    char *ports = copy_string(parameters.port);

    ConnectionStrings result = {
        .cnt = 0
    };


    while (host != nullptr) {
        if (result.cnt == MAX_HOSTS)
            raise_error("Too many hosts. Maximum value = %d", MAX_HOSTS);

        add_connection_string(&result, host, next_port(ports));
        host = next_host(hosts);
    }

    free(hosts);
    free(ports);

    return result;
}