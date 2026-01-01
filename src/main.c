#include "http_server.h"
#include "pg_monitor.h"
#include "utils.h"

#include <pthread.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <cjson/cJSON.h>

cJSON *host_to_json(const char *host) {
    cJSON *obj = json_object();
    if (!host)
        add_null_to_json_object(obj, "host");
    else
        add_str_to_json_object(obj, "host", host);
    return obj;
}

cJSON *replicas_to_json(const MonitorHost *cursor) {
    cJSON *arr = json_array();

    while (cursor) {
        const MonitorStatus *status = atomic_get_status(cursor);
        if (is_alive_replica(status))
            cJSON_AddItemToArray(arr, host_to_json(cursor -> host));
        cursor = cursor -> next;
    }

    return arr;
}

void get_replicas_json(HTTPResponse *response) {
    const MonitorHost *stat = get_monitor_host_head();
    cJSON *json = replicas_to_json(stat);

    response -> response = json_to_str(json);
    response -> memory_mode = MHD_RESPMEM_MUST_FREE;
    response -> content_type = "application/json";
}

void return_single_host(HTTPResponse *response, const char *host) {
    if (!host)
        response -> status_code = 404;

    if (need_json_response(response)) {
        response -> response = json_to_str(host_to_json(host));
        response -> memory_mode = MHD_RESPMEM_MUST_FREE;
    }
    else {
        response -> response = round_robin_replica();
        response -> memory_mode = MHD_RESPMEM_PERSISTENT;
    }
}

void get_random_replica(HTTPResponse *response) {
    const char *host = round_robin_replica();
    return_single_host(response, host);
}

void get_master(HTTPResponse *response) {
    const char *host = find_host(is_master, false);
    return_single_host(response, host);
}

void get_sync_host_by_time(HTTPResponse *response) {
    const char *host = find_host(is_sync_replica_by_time, true);
    return_single_host(response, host);
}

void get_sync_host_by_bytes(HTTPResponse *response) {
    const char *host = find_host(is_sync_replica_by_bytes, true);
    return_single_host(response, host);
}

void get_sync_host_by_time_or_bytes(HTTPResponse *response) {
    const char *host = find_host(is_sync_replica_by_time_or_bytes, true);
    return_single_host(response, host);
}

void get_sync_host_by_time_and_bytes(HTTPResponse *response) {
    const char *host = find_host(is_sync_replica_by_time_and_bytes, true);
    return_single_host(response, host);
}


int main(void) {
    sigset_t sigset;
    int sig;
    sigemptyset(&sigset);
    sigaddset(&sigset, SIGINT);
    sigaddset(&sigset, SIGTERM);

    if (pthread_sigmask(SIG_BLOCK, &sigset, nullptr) != 0) {
        perror("pthread_sigmask");
        return 1;
    }

    start_pg_monitor();

    Route routes[] = {
        { "GET", "/master", get_master },
        { "GET", "/replica", get_random_replica },
        { "GET", "/replicas_info", get_replicas_json },
        { "GET", "/sync_by_time", get_sync_host_by_time },
        { "GET", "/sync_by_bytes", get_sync_host_by_bytes },
        { "GET", "/sync_by_time_or_bytes", get_sync_host_by_time_or_bytes },
        { "GET", "/sync_by_time_and_bytes", get_sync_host_by_time_and_bytes },
    };
    MHD_Daemon *daemon = start_http_server(
        8000, routes, sizeof(routes) / sizeof(routes[0])
    );

    if (sigwait(&sigset, &sig) == 0) {
        if (sig == SIGINT)
            printf("SIGINT\n");
        else if (sig == SIGTERM)
            printf("SIGTERM\n");
    }

    stop_pg_monitor();
    stop_http_server(daemon);
    return 0;
}
