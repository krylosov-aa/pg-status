#include <stdio.h>
#include <stdlib.h>

#include "http_server.h"
#include "utils.h"
#include <string.h>

#include "pg_monitor.h"


MHD_Result index_get(HTTPResponse *response) {
    char *resp = "{\"Hello\": \"world\"}";
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
    monitor_thread();

    // Route routes[] = {
    //     { "GET", "/index", index_get }
    // };
    //
    // start_daemon(8000, routes, 1);
    return 0;
}
