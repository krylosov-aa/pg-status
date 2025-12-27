#include "http_server.h"
#include "pg_monitor.h"
#include "utils.h"

#include <pthread.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <cjson/cJSON.h>

cJSON *json_array(void) {
    cJSON *arr = cJSON_CreateArray();
    if (!arr)
        raise_error("Can't create json array");
    return arr;
}

cJSON *json_object(void) {
    cJSON *arr = cJSON_CreateObject();
    if (!arr)
        raise_error("Can't create json object");
    return arr;
}

void add_str_to_json_object(cJSON * obj, const char *key, const char *val) {
    if (!cJSON_AddStringToObject(obj, key, val)) {
        raise_error("Can't add str to object");
    }
}

char *json_to_str(cJSON *json) {
    char *result = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    return result;
}

cJSON *host_to_json(const char *host) {
    cJSON *obj = json_object();
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

MHD_Result get_replicas_json(HTTPResponse *response) {
    const MonitorHost *stat = get_monitor_host_head();
    cJSON *json = replicas_to_json(stat);

    response -> response = json_to_str(json);
    response -> memory_mode = MHD_RESPMEM_MUST_FREE;
    response -> status_code = MHD_HTTP_OK;

    if (!response -> content_type)
        response -> content_type = format_string("application/json");

    return MHD_YES;
}

MHD_Result get_random_replica_json(HTTPResponse *response) {
    char *host = round_robin_replica();
    cJSON *json = host_to_json(host);

    response -> response = json_to_str(json);
    response -> memory_mode = MHD_RESPMEM_MUST_FREE;
    response -> status_code = MHD_HTTP_OK;

    return MHD_YES;
}

MHD_Result get_random_replica(HTTPResponse *response) {
    if (
        response -> content_type &&
        is_equal_strings(response -> content_type, "application/json")
    )
        return get_random_replica_json(response);

    response -> response = round_robin_replica();
    response -> memory_mode = MHD_RESPMEM_PERSISTENT;
    response -> status_code = MHD_HTTP_OK;

    return MHD_YES;
}

MHD_Result get_master_json(HTTPResponse *response) {
    char *master = find_host(is_master, false);
    cJSON *json = host_to_json(master);

    response -> response = json_to_str(json);
    response -> memory_mode = MHD_RESPMEM_MUST_FREE;
    response -> status_code = MHD_HTTP_OK;

    return MHD_YES;
}

MHD_Result get_master(HTTPResponse *response) {
    if (
        response -> content_type &&
        is_equal_strings(response -> content_type, "application/json")
    )
        return get_master_json(response);

    response -> response = find_host(is_master, false);
    response -> memory_mode = MHD_RESPMEM_PERSISTENT;
    response -> status_code = MHD_HTTP_OK;

    return MHD_YES;
}

MHD_Result get_sync_host_by_time_json(HTTPResponse *response) {
    char *master = find_host(is_sync_replica_by_time, true);
    cJSON *json = host_to_json(master);

    response -> response = json_to_str(json);
    response -> memory_mode = MHD_RESPMEM_MUST_FREE;
    response -> status_code = MHD_HTTP_OK;

    return MHD_YES;
}

MHD_Result get_sync_host_by_time(HTTPResponse *response) {
    if (
        response -> content_type &&
        is_equal_strings(response -> content_type, "application/json")
    )
        return get_sync_host_by_time_json(response);

    response -> response = find_host(is_sync_replica_by_time, true);
    response -> memory_mode = MHD_RESPMEM_PERSISTENT;
    response -> status_code = MHD_HTTP_OK;

    return MHD_YES;
}

MHD_Result get_sync_host_by_bytes_json(HTTPResponse *response) {
    char *master = find_host(is_sync_replica_by_bytes, true);
    cJSON *json = host_to_json(master);

    response -> response = json_to_str(json);
    response -> memory_mode = MHD_RESPMEM_MUST_FREE;
    response -> status_code = MHD_HTTP_OK;

    return MHD_YES;
}

MHD_Result get_sync_host_by_bytes(HTTPResponse *response) {
    if (
        response -> content_type &&
        is_equal_strings(response -> content_type, "application/json")
    )
        return get_sync_host_by_bytes_json(response);

    response -> response = find_host(is_sync_replica_by_bytes, true);
    response -> memory_mode = MHD_RESPMEM_PERSISTENT;
    response -> status_code = MHD_HTTP_OK;

    return MHD_YES;
}

MHD_Result get_sync_host_by_time_or_bytes_json(HTTPResponse *response) {
    char *master = find_host(is_sync_replica_by_time_or_bytes, true);
    cJSON *json = host_to_json(master);

    response -> response = json_to_str(json);
    response -> memory_mode = MHD_RESPMEM_MUST_FREE;
    response -> status_code = MHD_HTTP_OK;

    return MHD_YES;
}

MHD_Result get_sync_host_by_time_or_bytes(HTTPResponse *response) {
    if (
        response -> content_type &&
        is_equal_strings(response -> content_type, "application/json")
    )
        return get_sync_host_by_time_or_bytes_json(response);

    response -> response = find_host(is_sync_replica_by_time_or_bytes, true);
    response -> memory_mode = MHD_RESPMEM_PERSISTENT;
    response -> status_code = MHD_HTTP_OK;

    return MHD_YES;
}

MHD_Result get_sync_host_by_time_and_bytes_json(HTTPResponse *response) {
    char *master = find_host(is_sync_replica_by_time_and_bytes, true);
    cJSON *json = host_to_json(master);

    response -> response = json_to_str(json);
    response -> memory_mode = MHD_RESPMEM_MUST_FREE;
    response -> status_code = MHD_HTTP_OK;

    return MHD_YES;
}

MHD_Result get_sync_host_by_time_and_bytes(HTTPResponse *response) {
    if (
        response -> content_type &&
        is_equal_strings(response -> content_type, "application/json")
    )
        return get_sync_host_by_time_and_bytes_json(response);

    response -> response = find_host(is_sync_replica_by_time_and_bytes, true);
    response -> memory_mode = MHD_RESPMEM_PERSISTENT;
    response -> status_code = MHD_HTTP_OK;

    return MHD_YES;
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
