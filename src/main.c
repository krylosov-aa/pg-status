#include "http_server.h"
#include "pg_monitor.h"
#include "utils.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>



MHD_Result master(HTTPResponse *response) {
    const MonitorStatus *cur_stat = get_cur_stat();
    char *resp = format_string("{\"host\": \"%s\"}", cur_stat -> master);
    MHD_Response *mhd_response = MHD_create_response_from_buffer(
        strlen(resp),
        (void*) resp,
        MHD_RESPMEM_MUST_FREE
    );
    response->mhd_response = mhd_response;
    response->status_code = MHD_HTTP_OK;
    return MHD_YES;
}

pthread_t start_pg_monitor() {
    pthread_t pg_monitor_tid;
    const int started = pthread_create(
        &pg_monitor_tid, nullptr, pg_monitor_thread, nullptr
    );

    if (started != 0)
        raise_error("Failed to start pg_monitor");

    return pg_monitor_tid;
}

int main(void) {
    const pthread_t pg_monitor_tid = start_pg_monitor();

    // while (1) {
    //     sleep(1);
    //
    //     const MonitorStatus *cur_stat = get_cur_stat();
    //
    //     if (cur_stat == nullptr) {
    //         printf("not ready\n");
    //         continue;
    //     }
    //
    //     printf("master: %s\n", cur_stat -> master);
    //     for (uint i = 0; i < cur_stat->replicas_cnt; i++) {
    //         printf("replica: %s\n", cur_stat -> replicas[i].host);
    //     }
    //     printf("\n");
    // }

    Route routes[] = {
        { "GET", "/master", master }
    };

    start_daemon(8000, routes, 1);

    // pthread_join(pg_monitor_tid, nullptr);
    return 0;
}
