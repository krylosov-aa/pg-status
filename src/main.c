#include "http_server.h"
#include "pg_monitor.h"
#include "utils.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>


MHD_Result get_master_json(HTTPResponse *response) {
    const MonitorStatus *cur_stat = get_cur_stat();
    char *resp = format_string("{\"host\": \"%s\"}", cur_stat -> master);
    MHD_Response *mhd_response = MHD_create_response_from_buffer(
        strlen(resp),
        (void*) resp,
        MHD_RESPMEM_MUST_FREE
    );
    response->mhd_response = mhd_response;
    response->status_code = MHD_HTTP_OK;
    response->content_type = "application/json";
    return MHD_YES;
}

MHD_Result get_master_plain(HTTPResponse *response) {
    const MonitorStatus *cur_stat = get_cur_stat();
    char *resp = cur_stat -> master;
    MHD_Response *mhd_response = MHD_create_response_from_buffer(
        strlen(resp),
        (void*) resp,
        MHD_RESPMEM_MUST_COPY
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
        { "GET", "/master_json", get_master_json },
        { "GET", "/master_plain", get_master_plain },
    };
    MHD_Daemon *daemon = start_http_server(8000, routes, 2);

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
