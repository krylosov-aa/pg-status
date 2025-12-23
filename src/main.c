#include "http_server.h"
#include "pg_monitor.h"
#include "utils.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <stdlib.h>
#include <cjson/cJSON.h>


cJSON *host_to_json(const char *host) {
    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        return nullptr;
    }

    if (!cJSON_AddStringToObject(obj, "host", host)) {
        cJSON_Delete(obj);
        return nullptr;
    }
    return obj;
}

cJSON *hosts_to_json(char **hosts, const size_t count) {
    cJSON *arr = cJSON_CreateArray();
    if (!arr)
        return nullptr;

    for (size_t i = 0; i < count; i++) {
        if (!hosts[i])
            continue;

        cJSON *obj = host_to_json(hosts[i]);
        if (!obj) {
            cJSON_Delete(arr);
            return nullptr;
        }

        cJSON_AddItemToArray(arr, obj);
    }

    return arr;
}

char *json_to_str(cJSON *json) {
    char *result = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    return result;
}

MHD_Result get_master_json(HTTPResponse *response) {
    const MonitorStatus *cur_stat = get_cur_stat();
    cJSON *json = host_to_json(cur_stat -> master);
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

    const MonitorStatus *cur_stat = get_cur_stat();
    char *resp = cur_stat -> master;
    MHD_Response *mhd_response = MHD_create_response_from_buffer(
        strlen(resp),
        (void*) resp,
        MHD_RESPMEM_PERSISTENT
    );
    response->mhd_response = mhd_response;
    response->status_code = MHD_HTTP_OK;
    return MHD_YES;
}


MHD_Result get_replicas_json(HTTPResponse *response) {
    const MonitorStatus *cur_stat = get_cur_stat();
    cJSON *json = hosts_to_json(cur_stat -> replicas, cur_stat -> cnt);
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
        { "GET", "/replicas_json", get_replicas_json },
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
