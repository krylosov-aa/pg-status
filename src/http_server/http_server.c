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
    MHD_Response *mhd_response = MHD_create_response_empty(MHD_RF_NONE);
    response->mhd_response = mhd_response;
    response->status_code = MHD_HTTP_NOT_FOUND;
    return MHD_YES;
}


MHD_Result queue_response(
    MHD_Connection *connection,
    const HTTPResponse *response,
    const char *path, const char *method
) {
    MHD_Result ret = MHD_NO;
    if (response->mhd_response) {
        MHD_add_response_header(
            response->mhd_response,
            "Content-Type",
            "application/json"
        );
        ret = MHD_queue_response(
            connection, response->status_code, response->mhd_response
        );
        if (ret == MHD_YES) {
            printf("%s %s\n", method, path);
        } else {
            ps_printf_error(
                "Failed to MHD_queue_response %s %s", method, path
            );
        }
        MHD_destroy_response(response->mhd_response);
    } else {
        ps_printf_error(
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
            ps_printf_error("request completed with error\n");
            break;
        case MHD_REQUEST_TERMINATED_TIMEOUT_REACHED:
            ps_printf_error("request completed with timeout\n");
            break;
        case MHD_REQUEST_TERMINATED_DAEMON_SHUTDOWN:
            ps_printf_error("request completed with MHD shutdown\n");
            break;
        case MHD_REQUEST_TERMINATED_READ_ERROR:
            ps_printf_error(
                "request completed with terminated read error\n"
            );
            break;
        case MHD_REQUEST_TERMINATED_CLIENT_ABORT:
            ps_printf_error("request completed with client abort\n");
            break;
    }
    if (*req_cls) {
        free(*req_cls);
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
    result = handler(response);

    if (response -> mhd_response) {
        result = queue_response(connection, response, path, method);
    }
    return result;
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
    HTTPResponse *response = malloc(sizeof(HTTPResponse));
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
        response = malloc(sizeof(HTTPResponse));
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


void start_daemon(
    const uint16_t port,
    Route *routes,
    const unsigned int cnt_routes
) {

    routes_list = malloc(sizeof(Routes));
    routes_list -> routes = routes;
    routes_list -> cnt = cnt_routes;

    MHD_Daemon *daemon = MHD_start_daemon(
        MHD_USE_AUTO_INTERNAL_THREAD
        | MHD_USE_ERROR_LOG,
        port, nullptr, nullptr,
        answer_to_connection, nullptr,
        // MHD_OPTION_THREAD_POOL_SIZE, 2,
        MHD_OPTION_NOTIFY_COMPLETED, request_completed, nullptr,
        // MHD_OPTION_NOTIFY_CONNECTION, notify_connection_callback, nullptr,
        MHD_OPTION_CONNECTION_MEMORY_LIMIT, 131072, // 128*1024
        MHD_OPTION_END
    );
    if (!daemon) {
        ps_raise_error("Failed to start mhd daemon");
    }
    printf("Daemon started at 127.0.0.1:%d\n", port);

    // const union MHD_DaemonInfo *daemon_info = MHD_get_daemon_info(
    //   daemon, MHD_DAEMON_INFO_CURRENT_CONNECTIONS
    // );
    // while (1) {
    //   printf("Current num_connections: %d\n", daemon_info -> num_connections);
    // }

    getchar();
    MHD_stop_daemon(daemon);
}
