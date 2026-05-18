/**
 * Monitoring of postgresql hosts
 */

#include "pg_monitor.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

#include "utils.h"

static pthread_t monitor_tid;

/**
 * Parameters for start a thread
 */
static pthread_mutex_t start_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t start_cond = PTHREAD_COND_INITIALIZER;
static bool pg_monitor_ready = false;

/**
 * Stop signaling via a pipe.
 */
static int stop_pipe[2] = {-1, -1};

/**
 * One iteration of host checking
 */
static void check_hosts(void) {
  int master_i = -1;
  int possible_master = -1;

  for (unsigned int i = 0; i < host_count; i++) {
    MonitorHost *item = &monitor_host_list[i];
    check_host_streaming_replication(item, parameters.max_fails);

    if (master_i == -1) {
      const MonitorStatus status = atomic_get_status(item);
      if (status.master) {
        if (!status.possible_dead) {
          master_i = (int)i;
          save_master_index(master_i);
        } else {
          possible_master = (int)i;
          // We don't need to update master_index if the master is marked
          // as possible dead, because it's already stored in there
        }
      }
    }
  }

  if (master_i == -1 && possible_master == -1) {
    save_master_index(-1);
  }

  update_hosts_cache();

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

  struct pollfd pfd = {
    .fd = stop_pipe[0],
    .events = POLLIN,
    .revents = 0,
  };

  for (;;) {
    const int rc = poll(&pfd, 1, parameters.sleep_ms);

    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      printf_error("poll failed");
      break;
    }

    if (rc > 0) {
      break;
    }

    check_hosts();
  }

  return nullptr;
}

/**
 * Starts a host monitoring thread
 */
pthread_t start_pg_monitor() {
  if (pipe(stop_pipe) != 0) {
    raise_error("Failed to create stop pipe");
  }
  if (fcntl(stop_pipe[0], F_SETFD, FD_CLOEXEC) != 0 ||
      fcntl(stop_pipe[1], F_SETFD, FD_CLOEXEC) != 0) {
    raise_error("Failed to set FD_CLOEXEC on stop pipe");
  }

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
  constexpr char b = 1;
  ssize_t w;
  do {
    w = write(stop_pipe[1], &b, 1);
  } while (w < 0 && errno == EINTR);

  pthread_join(monitor_tid, nullptr);

  close(stop_pipe[0]);
  close(stop_pipe[1]);
  stop_pipe[0] = -1;
  stop_pipe[1] = -1;

  free_hosts_cache();

  printf("pg_monitor stopped\n");
}
