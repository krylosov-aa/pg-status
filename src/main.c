#include <stdio.h>
#include <stdlib.h>

#include "http_server.h"
#include "utils.h"
#include <string.h>

#include "pg_monitor.h"


MHD_Result ps_index_get(PSHTTPResponse *response) {
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

    // ConnectionStrings con_str_list = get_connection_strings();
    // // for (uint i = 0; i < con_strs.cnt; i++) {
    // //     printf("%s\n", con_strs.connection_str[i]);
    // // }
    // check_hosts(con_str_list);
    //
    // // printf("%hhd\n", is_host_in_recovery());
    // PS_Route routes[] = {
    //     { "GET", "/index", ps_index_get }
    // };


    // ps_MHD_start_daemon(8000, routes, 1);
    return 0;
}
