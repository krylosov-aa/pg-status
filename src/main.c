#include "http_server.h"
#include "utils.h"
#include <string.h>


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
    PS_Route routes[] = {
        { "GET", "/index", ps_index_get }
    };


    ps_MHD_start_daemon(8000, routes, 1);
    return 0;
}
