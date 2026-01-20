/**
 * Monitoring of postgresql hosts
 */

#include "pg_monitor.h"
#include "utils.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
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
static atomic_bool monitor_running = true;

/**
 * Gets a flag indicating whether the monitor thread should keep running.
 * This is intentionally implemented using an atomic bool so that
 * even with sleep=0, the thread stops correctly, even if stop_pg_monitor
 * is starving while trying to acquire the lock
 */
static bool is_running(void) {
    return atomic_load_explicit(
        &monitor_running, memory_order_relaxed
    );
}

/**
 * One iteration of host checking
 */
static void check_hosts(void) {
    int master_i = -1;
    MonitorStatus master_status = {};

    for (int i = 0; i < host_count; i++) {
        MonitorHost *item = &monitor_host_list[i];
        check_host_streaming_replication(item, parameters.max_fails);

        if (master_i == -1  || master_status.possible_dead) {
            const MonitorStatus status = atomic_get_status(item);
            if (status.master) {
                master_i = i;
                master_status = status;
            }
        }

    }
    save_master_index(master_i);
    (void)fflush(stdout);
}

/**
 * The monitoring thread, which runs continuously and periodically
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
    while (is_running()) {
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += parameters.sleep;

        pthread_cond_timedwait(&stop_cond, &stop_mutex, &ts);
        if (!is_running()) {
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
    /*
     * This is intentionally implemented using an atomic bool so that
     * even with sleep=0, the thread stops correctly, even if stop_pg_monitor
     * is starving while trying to acquire the lock
    */
    atomic_store_explicit(
        &monitor_running, false, memory_order_relaxed
    );

    pthread_mutex_lock(&stop_mutex);
    pthread_cond_broadcast(&stop_cond);
    pthread_mutex_unlock(&stop_mutex);

    pthread_join(monitor_tid, nullptr);
    printf("pg_monitor stopped\n");
}
