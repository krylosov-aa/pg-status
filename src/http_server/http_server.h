#ifndef PG_STATUS_HTTP_SERVER_H
#define PG_STATUS_HTTP_SERVER_H

#include <microhttpd.h>


typedef struct MHD_Daemon MHD_Daemon;
typedef struct MHD_Connection MHD_Connection;
typedef struct MHD_Response MHD_Response;
typedef enum MHD_Result MHD_Result;
typedef enum MHD_RequestTerminationCode MHD_RequestTerminationCode;

typedef struct HTTPResponse {
    MHD_Response *mhd_response;
    unsigned int status_code;
} HTTPResponse;

typedef MHD_Result (*request_handler_t)(HTTPResponse *response);

typedef struct Route {
    const char *method;
    const char *path;
    request_handler_t handler;
} Route;

void start_daemon(
    const uint16_t port,
    Route *routes,
    const unsigned int cnt_routes
);


#endif //PG_STATUS_HTTP_SERVER_H
