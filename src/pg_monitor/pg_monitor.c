/**
 * Monitoring of postgresql hosts
 */

#include "pg_monitor.h"
#include "utils.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>


static pthread_t monitor_tid;

/**
 * Parameters for start a thread
 */
static pthread_mutex_t start_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  start_cond  = PTHREAD_COND_INITIALIZER;
static bool pg_monitor_ready = false;

/**
 * Parameters for stop a thread
 */
static pthread_mutex_t stop_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  stop_cond  = PTHREAD_COND_INITIALIZER;
static bool monitor_running = true;

/**
 * One iteration of host checking
 */
static void check_hosts(void) {
    char *master = nullptr;
    for (uint8_t i = 0; i < host_count; i++) {
        MonitorHost *item = &monitor_host_list[i];
        check_host_streaming_replication(item, parameters.max_fails);

        if (!master) {
            const MonitorStatus status = atomic_get_status(item);
            if (status.master) {
                master = item -> host;
            }
        }

    }
    save_master_host(master);
    (void)fflush(stdout);
}

/**
 * The main monitoring thread, which runs continuously and periodically
 * does host checks
 */
static void *pg_monitor_thread(void *arg) {
    set_parameters_from_env();
    init_monitor_host_list();

    check_hosts();

    pthread_mutex_lock(&start_mutex);
    pg_monitor_ready = true;
    pthread_cond_broadcast(&start_cond);
    pthread_mutex_unlock(&start_mutex);

    struct timespec ts;
    pthread_mutex_lock(&stop_mutex);
    while (monitor_running) {
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += parameters.sleep;

        pthread_cond_timedwait(&stop_cond, &stop_mutex, &ts);
        if (!monitor_running) {
            break;
        };

        check_hosts();
    }
    pthread_mutex_unlock(&stop_mutex);
    return nullptr;
}

/**
 * Starts a host monitoring thread
 */
pthread_t start_pg_monitor() {
    pthread_mutex_lock(&start_mutex);
    const int started = pthread_create(
        &monitor_tid, nullptr, pg_monitor_thread, nullptr
    );

    if (started != 0) {
        pthread_mutex_unlock(&start_mutex);
        raise_error("Failed to start pg_monitor");
    }

    while (!pg_monitor_ready) {
        pthread_cond_wait(&start_cond, &start_mutex);
    }
    pthread_mutex_unlock(&start_mutex);
    printf("pg_monitor started\n");
    return monitor_tid;
}

/**
 * Stops a host monitoring thread
 */
void stop_pg_monitor(void) {
    pthread_mutex_lock(&stop_mutex);
    monitor_running = false;
    pthread_cond_broadcast(&stop_cond);
    pthread_mutex_unlock(&stop_mutex);

    pthread_join(monitor_tid, nullptr);
    printf("pg_monitor stopped\n");
}
