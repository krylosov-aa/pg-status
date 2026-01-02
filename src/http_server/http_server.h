#ifndef PG_STATUS_HTTP_SERVER_H
#define PG_STATUS_HTTP_SERVER_H

#include <microhttpd.h>


typedef struct MHD_Daemon MHD_Daemon;
typedef struct MHD_Connection MHD_Connection;
typedef struct MHD_Response MHD_Response;
typedef enum MHD_Result MHD_Result;
typedef enum MHD_RequestTerminationCode MHD_RequestTerminationCode;
typedef enum MHD_ResponseMemoryMode MHD_ResponseMemoryMode;

/**
 * Structure for convenient response formation
 */
typedef struct HTTPResponse {
    // For simple cases, just form a string, and the server itself will
    // convert it to MHD_Response
    char *response;

    // For complex cases, you can manually generate MHD_Response
    MHD_Response *mhd_response;

    // Response type. It can be set by the server or specified by handler.
    // Must be allocated on the heap and will be freed by the server.
    const char *content_type;

    // What to do with the response after mhd gives the response to the client
    MHD_ResponseMemoryMode memory_mode;

    // Response status
    unsigned int status_code;
} HTTPResponse;

/**
 * Interface for the handler that will be called when the route is called
 */
typedef void (*request_handler_t)(HTTPResponse *response);

/**
 * A route that specifies the path, method, and callable
 * handler that will be called when accessing this
 * route to generate a response.
 */
typedef struct Route {
    const char *method;
    const char *path;
    request_handler_t handler;
} Route;

/**
 * Starts http server daemon
 */
MHD_Daemon *start_http_server(
    uint16_t port,
    Route *routes,
    unsigned int cnt_routes
);

/**
 * Stops http server daemon
 */
void stop_http_server(MHD_Daemon *daemon);

bool need_json_response(const HTTPResponse *response);

#endif //PG_STATUS_HTTP_SERVER_H
