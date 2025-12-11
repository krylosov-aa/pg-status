#include "http_server.h"

#include "utils.h"
#include <microhttpd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


MHD_Result ps_not_found(PSHTTPResponse *response) {
    MHD_Response *mhd_response = MHD_create_response_from_buffer(
        0, (void*)"", MHD_RESPMEM_PERSISTENT
    );
    response->mhd_response = mhd_response;
    response->status_code = MHD_HTTP_NOT_FOUND;
    return MHD_YES;
}


MHD_Result ps_queue_response(
    MHD_Connection *connection,
    const PSHTTPResponse *response,
    const char *url, const char *method
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
            printf("response for %s %s is queued", method, url);
        } else {
            ps_printf_error("Failed to MHD_queue_response %s %s", method, url);
        }
        MHD_destroy_response(response->mhd_response);
    } else {
        ps_printf_error("mhd_response wasn't provided for MHD_queue_response %s %s", method, url);
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
            printf("request completed\n");
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
            ps_printf_error("request completed with terminated read error\n");
            break;
        case MHD_REQUEST_TERMINATED_CLIENT_ABORT:
            ps_printf_error("request completed with client abort\n");
            break;
    }
    if (*req_cls) {
        free(*req_cls);
    }
}

void ps_mhd_notify_connection_callback(
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

void ps_MHD_start_daemon(
    uint16_t port,
    const MHD_AccessHandlerCallback dh
) {
    MHD_Daemon *daemon = MHD_start_daemon(
        MHD_USE_AUTO_INTERNAL_THREAD
        // | MHD_USE_EPOLL_INTERNAL_THREAD
        | MHD_USE_ERROR_LOG,
        port, NULL, NULL,
        dh, NULL,
        MHD_OPTION_THREAD_POOL_SIZE, 4,
        MHD_OPTION_NOTIFY_COMPLETED, request_completed, NULL,
        MHD_OPTION_NOTIFY_CONNECTION, ps_mhd_notify_connection_callback, NULL,
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
