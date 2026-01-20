#include "http_server.h"
#include "pg_monitor.h"
#include "utils.h"

#include <pthread.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <cjson/cJSON.h>

static void add_host_to_json(cJSON *json_obj, const char *host) {
    if (!host) {
        add_null_to_json_object(json_obj, "host");
    }
    else {
        add_str_to_json_object(json_obj, "host", host);
    }
}

void get_all_hosts(MHD_Connection *connection, HTTPResponse *response) {
    cJSON *arr = json_array();
    for (uint8_t i = 0; i < host_count; i++) {
        const MonitorHost *mon_host = &monitor_host_list[i];
        cJSON *json_obj = json_object();
        add_host_to_json(json_obj, mon_host -> host);

        const MonitorStatus status = atomic_get_status(mon_host);
        add_bool_to_json_object(json_obj, "master", status.master);
        add_bool_to_json_object(json_obj, "alive", status.alive);

        cJSON_AddItemToArray(arr, json_obj);
    }

    response -> response = json_to_str(arr);
    response -> memory_mode = MHD_RESPMEM_MUST_FREE;
    response -> content_type = "application/json";
}

static void return_single_host(
    HTTPResponse *response, const char *host
) {
    if (!host) {
        response -> status_code = 404;
    }

    if (need_json_response(response)) {
        cJSON *json_obj = json_object();
        add_host_to_json(json_obj, host);
        response -> response = json_to_str(json_obj);
        response -> memory_mode = MHD_RESPMEM_MUST_FREE;
    }
    else {
        response -> const_response = host;
    }
}

static void get_random_replica(MHD_Connection *connection, HTTPResponse *response) {
    const char *host = find_replica_round_robin(is_alive_replica, true);
    return_single_host(response, host);
}

static void get_master(MHD_Connection *connection, HTTPResponse *response) {
    const char *host = get_master_host();
    return_single_host(response, host);
}

static void get_sync_host_by_time(
    MHD_Connection *connection, HTTPResponse *response
) {
    const char *host = find_replica_round_robin(is_sync_replica_by_time, true);
    return_single_host(response, host);
}

static void get_sync_host_by_bytes(
    MHD_Connection *connection, HTTPResponse *response
) {
    const char *host = find_replica_round_robin(is_sync_replica_by_bytes, true);
    return_single_host(response, host);
}

static void get_sync_host_by_time_or_bytes(
    MHD_Connection *connection, HTTPResponse *response
) {
    const char *host = find_replica_round_robin(
        is_sync_replica_by_time_or_bytes, true
    );
    return_single_host(response, host);
}

static void get_sync_host_by_time_and_bytes(
    MHD_Connection *connection, HTTPResponse *response
) {
    const char *host = find_replica_round_robin(
        is_sync_replica_by_time_and_bytes, true
    );
    return_single_host(response, host);
}


static void get_host_status(MHD_Connection *connection, HTTPResponse *response) {
    const char *host = MHD_lookup_connection_value(
        connection,
        MHD_GET_ARGUMENT_KIND,
        "host"
    );
    if (!host) {
        response -> status_code = 400;
        response -> const_response = "{\"error_text\": \"Get parameter 'host' wasn't passed\"}";
        return;
    }
    const MonitorHost *mon_host = find_host_by_name(host);
    if (!mon_host) {
        response -> status_code = 404;
        return;
    }

    const MonitorStatus status = atomic_get_status(mon_host);
    cJSON *json_obj = json_object();
    add_bool_to_json_object(json_obj, "master", status.master);
    add_bool_to_json_object(json_obj, "alive", status.alive);
    add_bool_to_json_object(json_obj, "sync_by_time", status.sync_by_time);
    add_bool_to_json_object(json_obj, "sync_by_bytes", status.sync_by_bytes);

    response -> response = json_to_str(json_obj);
    response -> memory_mode = MHD_RESPMEM_MUST_FREE;
    response -> content_type = "application/json";
}

static void block_termination_signals(sigset_t *sigset) {
    sigemptyset(sigset);
    sigaddset(sigset, SIGINT);
    sigaddset(sigset, SIGTERM);

    if (pthread_sigmask(SIG_BLOCK, sigset, NULL) != 0) {
        raise_error("pthread_sigmask");
    }
}


static void get_version(
    MHD_Connection *connection, HTTPResponse *response
) {
    response -> const_response = "1.6.0";
}


static void wait_for_termination_signal(const sigset_t *sigset) {
    int sig;

    if (sigwait(sigset, &sig) == 0) {
        switch (sig) {
            case SIGINT:
                printf("Received SIGINT\n");
                break;
            case SIGTERM:
                printf("Received SIGTERM\n");
                break;
            default:
                printf("Received signal %d\n", sig);
        }
    }
}


static uint16_t get_port() {
    const char *env_val = getenv("pg_status__http_port");
    if (env_val && *env_val) {
        return str_to_uint16(getenv("pg_status__http_port"));
    }
    return 8000;
}

static Route routes[] = {
    { "GET", "/master", get_master },
    { "GET", "/replica", get_random_replica },
    { "GET", "/hosts", get_all_hosts },
    { "GET", "/status", get_host_status },
    { "GET", "/sync_by_time", get_sync_host_by_time },
    { "GET", "/sync_by_bytes", get_sync_host_by_bytes },
    { "GET", "/sync_by_time_or_bytes", get_sync_host_by_time_or_bytes },
    { "GET", "/sync_by_time_and_bytes", get_sync_host_by_time_and_bytes },
    { "GET", "/version", get_version },
};

int main() {
    sigset_t sigset;
    block_termination_signals(&sigset);

    start_pg_monitor();
    MHD_Daemon *daemon = start_http_server(
        get_port(), routes, sizeof(routes) / sizeof(routes[0])
    );

    wait_for_termination_signal(&sigset);

    stop_pg_monitor();
    stop_http_server(daemon);
    return 0;
}
