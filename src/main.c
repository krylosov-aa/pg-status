#include "http_server.h"
#include "pg_monitor.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
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


MHD_Result answer_to_connection(
  void *cls,
  MHD_Connection *connection,
  const char *url,
  const char *method,
  const char *version,
  const char *upload_data,
  size_t *upload_data_size,
  void **req_cls
) {
    printf("start answer_to_connection %s %s\n", method, url);
    MHD_Result result = MHD_NO;

    PSHTTPResponse *response = NULL;
    if (*req_cls == NULL) {
        response = malloc(sizeof(PSHTTPResponse));
        *req_cls = (void *) response;
        return MHD_YES;
    }

    response = (PSHTTPResponse *) *req_cls;

    if (ps_is_equal_strings(url, "/index")) {
        if (ps_is_equal_strings(method, "GET")) {
            result = ps_index_get(response);
        }
    }
    else {
        result = ps_not_found(response);
    }
    if (response -> mhd_response) {
        result = ps_queue_response(connection, response, url, method);
    }

    printf("end answer_to_connection %s %s with %d\n", method, url, result);
    return result;
}


int main(void) {
    ps_MHD_start_daemon(8000, &answer_to_connection);
    return 0;
}
