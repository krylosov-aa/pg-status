#include "http_server.h"
#include "pg_monitor.h"
#include "utils.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>



MHD_Result get_master(HTTPResponse *response) {
    const MonitorStatus *cur_stat = get_cur_stat();
    char *resp = format_string("{\"host\": \"%s\"}", cur_stat -> master);
    MHD_Response *mhd_response = MHD_create_response_from_buffer(
        strlen(resp),
        (void*) resp,
        MHD_RESPMEM_MUST_FREE
    );
    response->mhd_response = mhd_response;
    response->status_code = MHD_HTTP_OK;
    return MHD_YES;
}


int main(void) {
    start_pg_monitor();

    Route routes[] = {
        { "GET", "/master", get_master }
    };
    MHD_Daemon *daemon = start_http_server(8000, routes, 1);

    getchar();

    stop_pg_monitor();
    stop_http_server(daemon);
    return 0;
}
