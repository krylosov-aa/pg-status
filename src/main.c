#include "http_server.h"
#include "pg_monitor.h"
#include "utils.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
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

bool is_alive_replica(const MonitorStatus *host) {
    return (
        get_bool_atomic(&host -> alive) &&
        !get_bool_atomic(&host -> is_master)
    );
}

cJSON *replicas_to_json(const MonitorStatus *cursor) {
    cJSON *arr = json_array();

    while (cursor) {
        if (is_alive_replica(cursor))
            cJSON_AddItemToArray(arr, host_to_json(cursor -> host));
        cursor = cursor -> next;
    }

    return arr;
}


MHD_Result get_replicas_json(HTTPResponse *response) {
    const MonitorStatus *stat = get_monitor_status();
    cJSON *json = replicas_to_json(stat);
    char *resp = json_to_str(json);
    MHD_Response *mhd_response = MHD_create_response_from_buffer(
        strlen(resp),
        (void*) resp,
        MHD_RESPMEM_MUST_FREE
    );
    response->mhd_response = mhd_response;
    response->status_code = MHD_HTTP_OK;
    response->content_type = format_string("application/json");
    return MHD_YES;
}

_Atomic (MonitorStatus *) last_random_replica = nullptr;

char *round_robin_replica(void) {
    MonitorStatus *cursor = atomic_load_explicit(
        &last_random_replica, memory_order_acquire
    );
    if (!cursor || !cursor -> next)
        cursor = get_monitor_status();
    else
        cursor = cursor -> next;

    unsigned int i = 0;
    while (!is_alive_replica(cursor)) {
        cursor = cursor -> next;
        if (!cursor)
            cursor = get_monitor_status();
        i++;
        if (i == MAX_HOSTS)
            break;
    }
    atomic_store_explicit(&last_random_replica, cursor, memory_order_release);

    return i < MAX_HOSTS ? cursor -> host: "null";
}

MHD_Result get_random_replica_json(HTTPResponse *response) {
    char *host = round_robin_replica();
    cJSON *json = host_to_json(host);
    char *resp = json_to_str(json);
    MHD_Response *mhd_response = MHD_create_response_from_buffer(
        strlen(resp),
        (void*) resp,
        MHD_RESPMEM_PERSISTENT
    );
    response->mhd_response = mhd_response;
    response->status_code = MHD_HTTP_OK;
    return MHD_YES;
}

MHD_Result get_random_replica(HTTPResponse *response) {
    if (
        response -> content_type &&
        is_equal_strings(response -> content_type, "application/json")
    )
        return get_random_replica_json(response);

    char *resp = round_robin_replica();
    MHD_Response *mhd_response = MHD_create_response_from_buffer(
        strlen(resp),
        (void*) resp,
        MHD_RESPMEM_PERSISTENT
    );
    response->mhd_response = mhd_response;
    response->status_code = MHD_HTTP_OK;
    return MHD_YES;
}

MHD_Result get_master_json(HTTPResponse *response) {
    char *master = get_master_host();
    cJSON *json = host_to_json(master);
    char *resp = json_to_str(json);
    MHD_Response *mhd_response = MHD_create_response_from_buffer(
        strlen(resp),
        (void*) resp,
        MHD_RESPMEM_MUST_FREE
    );
    response->mhd_response = mhd_response;
    response->status_code = MHD_HTTP_OK;
    return MHD_YES;
}

MHD_Result get_master(HTTPResponse *response) {
    if (
        response -> content_type &&
        is_equal_strings(response -> content_type, "application/json")
    )
        return get_master_json(response);

    char *resp = get_master_host();
    MHD_Response *mhd_response = MHD_create_response_from_buffer(
        strlen(resp),
        (void*) resp,
        MHD_RESPMEM_PERSISTENT
    );
    response->mhd_response = mhd_response;
    response->status_code = MHD_HTTP_OK;
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
        { "GET", "/replicas", get_replicas_json },
        { "GET", "/replica", get_random_replica },
    };
    MHD_Daemon *daemon = start_http_server(8000, routes, 3);

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
