#include "http_server.h"

#include "utils.h"
#include <microhttpd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Routes {
    Route *routes;
    unsigned int cnt;
} Routes;

Routes *routes_list = nullptr;


MHD_Result not_found(HTTPResponse *response) {
    MHD_Response *mhd_response = MHD_create_response_from_buffer(
        0, NULL, MHD_RESPMEM_PERSISTENT
    );
    response->mhd_response = mhd_response;
    response->status_code = MHD_HTTP_NOT_FOUND;
    return MHD_YES;
}

void set_content_type_header(const HTTPResponse *response) {
    if (response != nullptr && response -> content_type != nullptr) {
        MHD_add_response_header(
            response->mhd_response,
            MHD_HTTP_HEADER_CONTENT_TYPE,
            response -> content_type
        );
    }
}

MHD_Result queue_response(
    MHD_Connection *connection,
    const HTTPResponse *response,
    const char *path, const char *method
) {
    MHD_Result ret = MHD_NO;
    if (response->mhd_response) {
        set_content_type_header(response);
        ret = MHD_queue_response(
            connection, response->status_code, response->mhd_response
        );
        if (ret == MHD_YES) {
            // printf("%s %s\n", method, path);
        } else {
            printf_error(
                "Failed to MHD_queue_response %s %s", method, path
            );
        }
        MHD_destroy_response(response->mhd_response);
    } else {
        printf_error(
            "mhd_response wasn't provided for MHD_queue_response %s %s",
            method, path);
    }
    return ret;
}

void request_completed(
    void *cls,
    MHD_Connection *connection,
    void **req_cls,
    const MHD_RequestTerminationCode toe
) {
    switch (toe) {
        case MHD_REQUEST_TERMINATED_COMPLETED_OK:
            // printf("request completed\n");
            break;
        case MHD_REQUEST_TERMINATED_WITH_ERROR:
            printf_error("request completed with error\n");
            break;
        case MHD_REQUEST_TERMINATED_TIMEOUT_REACHED:
            printf_error("request completed with timeout\n");
            break;
        case MHD_REQUEST_TERMINATED_DAEMON_SHUTDOWN:
            printf_error("request completed with MHD shutdown\n");
            break;
        case MHD_REQUEST_TERMINATED_READ_ERROR:
            printf_error(
                "request completed with terminated read error\n"
            );
            break;
        case MHD_REQUEST_TERMINATED_CLIENT_ABORT:
            printf_error("request completed with client abort\n");
            break;
    }
    if (*req_cls) {
        HTTPResponse *response = *req_cls;
        char *content_type = response -> content_type;
        if (content_type != nullptr)
            free(content_type);
        free(response);
    }
}

void notify_connection_callback(
    void *cls,
    MHD_Connection *connection,
    void **socket_context,
    enum MHD_ConnectionNotificationCode toe
) {
    switch (toe) {
        case MHD_CONNECTION_NOTIFY_STARTED:
            printf("Connection started\n");
            break;
        case MHD_CONNECTION_NOTIFY_CLOSED:
            printf("Connection closed\n");
            break;
    }
}

request_handler_t find_handler(const char *method, const char *path) {
    for (unsigned int i = 0; i < routes_list -> cnt; i++) {
        Route *routes = routes_list -> routes;

        if (strcmp(routes[i].method, method) == 0 &&
            strcmp(routes[i].path, path) == 0)
            return routes[i].handler;
    }
    return not_found;
}

MHD_Result process_handler(
  const char *path,
  const char *method,
  HTTPResponse *response,
  MHD_Connection *connection
) {
    MHD_Result result = MHD_NO;
    const request_handler_t handler = find_handler(method, path);

    const char *content_type = MHD_lookup_connection_value(
        connection, MHD_HEADER_KIND, MHD_HTTP_HEADER_CONTENT_TYPE
    );
    if (content_type != nullptr)
        response -> content_type = copy_string(content_type);

    result = handler(response);

    if (response -> mhd_response) {
        result = queue_response(connection, response, path, method);
    }
    return result;
}


HTTPResponse *allocate_response(void) {
    HTTPResponse *response = malloc(sizeof(HTTPResponse));
    if (response != nullptr) {
        response -> mhd_response = nullptr;
        response -> content_type = nullptr;
        response -> status_code = 200;
    }
    return response;
}

MHD_Result process_get(
  void *cls,
  MHD_Connection *connection,
  const char *path,
  const char *method,
  const char *version,
  const char *upload_data,
  void **req_cls
) {
    HTTPResponse *response = allocate_response();
    *req_cls = (void *) response;

    return process_handler(path, method, response, connection);
}

MHD_Result process_post(
  void *cls,
  MHD_Connection *connection,
  const char *path,
  const char *method,
  const char *version,
  const char *upload_data,
  size_t *upload_data_size,
  void **req_cls
) {
    HTTPResponse *response;
    if (*req_cls == nullptr) {
        response = allocate_response();
        *req_cls = (void *) response;
        return MHD_YES;
    }
    response = (HTTPResponse *) *req_cls;

    return process_handler(path, method, response, connection);
}

MHD_Result answer_to_connection(
  void *cls,
  MHD_Connection *connection,
  const char *path,
  const char *method,
  const char *version,
  const char *upload_data,
  size_t *upload_data_size,
  void **req_cls
) {
    // pg-status always receive get requests
    // we don't need two-step processing like we do for the POST requests
    return process_get(
        cls, connection, path, method, version, upload_data, req_cls
    );
}


MHD_Daemon *start_http_server(
    const uint16_t port,
    Route *routes,
    const unsigned int cnt_routes
) {

    routes_list = malloc(sizeof(Routes));
    routes_list -> routes = routes;
    routes_list -> cnt = cnt_routes;

    MHD_Daemon *daemon = MHD_start_daemon(
        MHD_USE_AUTO_INTERNAL_THREAD |
        MHD_USE_ERROR_LOG,
        port, nullptr, nullptr,
        answer_to_connection, nullptr,
        MHD_OPTION_NOTIFY_COMPLETED, request_completed, nullptr,
        // MHD_OPTION_NOTIFY_CONNECTION, notify_connection_callback, nullptr,
        MHD_OPTION_CONNECTION_MEMORY_LIMIT, 16 * 1024,
        MHD_OPTION_END
    );
    if (!daemon) {
        raise_error("Failed to start mhd daemon");
    }
    printf("http server started at 127.0.0.1:%d\n", port);
    return daemon;

}

void stop_http_server(MHD_Daemon *daemon) {
    MHD_stop_daemon(daemon);
    printf("http server stopped\n");
}
