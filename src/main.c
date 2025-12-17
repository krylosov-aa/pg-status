#include "http_server.h"
#include "pg_monitor.h"
#include "utils.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>



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
    start_pg_monitor();

    Route routes[] = {
        { "GET", "/master_json", get_master_json },
        { "GET", "/master_plain", get_master_plain },
    };
    MHD_Daemon *daemon = start_http_server(8000, routes, 2);

    getchar();

    stop_pg_monitor();
    stop_http_server(daemon);
    return 0;
}
