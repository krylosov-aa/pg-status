#ifndef PG_STATUS_HTTP_SERVER_H
#define PG_STATUS_HTTP_SERVER_H

#include <microhttpd.h>


typedef struct MHD_Daemon MHD_Daemon;
typedef struct MHD_Connection MHD_Connection;
typedef struct MHD_Response MHD_Response;
typedef enum MHD_Result MHD_Result;
typedef enum MHD_RequestTerminationCode MHD_RequestTerminationCode;

typedef struct PSHTTPResponse {
    MHD_Response *mhd_response;
    unsigned int status_code;
} PSHTTPResponse;

typedef MHD_Result (*request_handler_t)(PSHTTPResponse *response);

typedef struct PS_Route {
    const char *method;
    const char *path;
    request_handler_t handler;
} PS_Route;


typedef struct PSHTTPConnection {
    PSHTTPResponse response;
    char *payload;
} PSHTTPConnection;

MHD_Result ps_not_found(PSHTTPResponse *response);

typedef MHD_Result (*ps_create_response_not_found_call_back)(PSHTTPResponse *response);

typedef MHD_Result (*ps_create_response_error_call_back)(PSHTTPResponse *response);

void ps_MHD_start_daemon(
    const uint16_t port,
    PS_Route *routes,
    const unsigned int cnt_routes
) ;

MHD_Result ps_queue_response(
    MHD_Connection *connection,
    const PSHTTPResponse *response,
    const char *url, const char *method
);


#endif //PG_STATUS_HTTP_SERVER_H
