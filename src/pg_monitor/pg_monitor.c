#include "pg_monitor.h"
#include "utils.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>


_Atomic(MonitorStatus *) monitor_status = nullptr;

MonitorStatus buffers[2];

void init_buffers(const unsigned int hosts_cnt) {
    for (int i = 0; i < 2; i++) {
        buffers[i].master = "null";
        buffers[i].replicas = calloc(hosts_cnt, sizeof(char *));
        buffers[i].liveness = calloc(hosts_cnt, sizeof(HostLiveness));
        buffers[i].cnt = hosts_cnt;
    }

    atomic_store_explicit(&monitor_status, &buffers[0], memory_order_release);
}

void reset_inactive(MonitorStatus *inactive) {
    inactive -> master = "null";
    memset(
        inactive -> replicas,
        0,
        inactive -> cnt * sizeof(char *)
    );
    memset(
        inactive -> liveness,
        0,
        inactive -> cnt * sizeof(HostLiveness)
    );
}

MonitorStatus *prep_inactive(void) {
    const MonitorStatus *active = atomic_load_explicit(
        &monitor_status, memory_order_acquire
    );
    MonitorStatus *inactive = active == &buffers[0] ? &buffers[1] : &buffers[0];
    reset_inactive(inactive);
    return inactive;
}

void check_hosts(const ConnectionStrings con_str_list) {
    MonitorStatus *inactive = prep_inactive();

    char **replicas_cursor = inactive -> replicas;
    HostLiveness *liveness_cursor = inactive -> liveness;

    for (uint i = 0; i < con_str_list.cnt; i++) {
        bool is_replica = true;
        char *host = con_str_list.hosts[i];
        const int exit_val = is_host_in_recovery(
            con_str_list.connection_str[i],
            &is_replica
        );

        if (exit_val == 0) {
            liveness_cursor -> host = host;
            liveness_cursor -> alive = true;
            liveness_cursor++;

            if (is_replica) {
                printf("%s: replica\n", con_str_list.hosts[i]);
                *replicas_cursor = host;
                replicas_cursor++;
            }
            else {
                printf("%s: master\n", con_str_list.hosts[i]);
                inactive -> master = host;
            }
        }
        else {
            printf("%s: dead\n", con_str_list.hosts[i]);
            liveness_cursor -> host = host;
            liveness_cursor -> alive = false;
            liveness_cursor++;
        }
    }
    printf("\n");

    atomic_store_explicit(&monitor_status, inactive, memory_order_release);
}

const MonitorStatus *get_cur_stat(void) {
    return atomic_load_explicit(
        &monitor_status, memory_order_acquire
    );
}

atomic_uint pg_monitor_running = 1;

void stop_pg_monitor(void) {
    atomic_store(&pg_monitor_running, true);
    printf("pg_monitor stopped\n");
}

void *pg_monitor_thread(void *arg) {
    (void)arg;

    const ConnectionStrings con_str_list = get_connection_strings();
    init_buffers(con_str_list.cnt);

    while (atomic_load(&pg_monitor_running)) {
        check_hosts(con_str_list);
        sleep(parameters.sleep);
    }
    return nullptr;
}

pthread_t start_pg_monitor() {
    pthread_t pg_monitor_tid;
    const int started = pthread_create(
        &pg_monitor_tid, nullptr, pg_monitor_thread, nullptr
    );

    if (started != 0)
        raise_error("Failed to start pg_monitor");

    printf("pg_monitor started\n");
    return pg_monitor_tid;
}
