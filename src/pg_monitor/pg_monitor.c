/**
 * Monitoring of postgresql hosts
 */

#include "pg_monitor.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <sys/types.h>
#include <unistd.h>

#include "logger.h"
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
 * Picks the current master by scanning hosts in declared order:
 * first fully-alive master wins; otherwise the previously stored
 * master_index is left in place if any host is marked possible_dead
 * master; only when no master exists at all is master_index cleared
 * to -1. Same rules the synchronous loop used to apply.
 */
static void recompute_master_index(void) {
  int master_i = -1;
  int possible_master = -1;
  for (unsigned int i = 0; i < host_count; i++) {
    const MonitorStatus status = atomic_get_status(&monitor_host_list[i]);
    if (!status.master) {
      continue;
    }
    if (!status.possible_dead) {
      master_i = (int)i;
      break;
    }
    if (possible_master == -1) {
      possible_master = (int)i;
    }
  }
  if (master_i != -1) {
    save_master_index(master_i);
  } else if (possible_master == -1) {
    save_master_index(-1);
  }
}

/**
 * True once every host has completed at least one poll iteration
 * (success or failure). Used to gate startup readiness.
 */
static bool all_hosts_have_polled(void) {
  for (unsigned int i = 0; i < host_count; i++) {
    if (monitor_host_list[i].next_poll_at_ms == 0) {
      return false;
    }
  }
  return true;
}

/**
 * Starts any IDLE host whose next_poll_at_ms has elapsed; time out
 * any non-IDLE host whose iter_deadline_ms has passed.
 */
static void start_and_timeout_hosts(const uint64_t now_ms) {
  for (unsigned int i = 0; i < host_count; i++) {
    MonitorHost *host = &monitor_host_list[i];
    if (host->poll_state == HOST_POLL_IDLE) {
      if (now_ms >= host->next_poll_at_ms) {
        start_host_poll(host, now_ms);
      }
    } else if (now_ms >= host->iter_deadline_ms) {
      timeout_host_poll(host, now_ms);
    }
  }
}

/**
 * Builds pollfd[] across the stop pipe and all in-flight hosts.
 */
static int build_poll_fd(
  const uint64_t now_ms, struct pollfd *pfds, int *timeout_ms
) {
  const struct pollfd stop_pipe_slot = {
    .fd = stop_pipe[0], .events = POLLIN, .revents = 0
  };
  pfds[0] = stop_pipe_slot;

  uint64_t soonest_wake = now_ms + (uint64_t)parameters.sleep_ms;

  int n_pfds = 1;
  for (unsigned int i = 0; i < host_count; i++) {
    MonitorHost *host = &monitor_host_list[i];

    if (host->poll_state == HOST_POLL_IDLE) {
      if (host->next_poll_at_ms < soonest_wake) {
        soonest_wake = host->next_poll_at_ms;
      }
      host->pollfd_slot = -1;
      continue;
    }

    const int fd = host_socket(host);
    if (fd < 0) {
      // Connection vanished mid-iteration; force a timeout next tick.
      host->iter_deadline_ms = now_ms;
      host->pollfd_slot = -1;
      continue;
    }

    if (host->iter_deadline_ms < soonest_wake) {
      soonest_wake = host->iter_deadline_ms;
    }

    pfds[n_pfds] = (struct pollfd){
      .fd = fd, .events = host->poll_events, .revents = 0
    };
    host->pollfd_slot = n_pfds;

    n_pfds++;
  }

  const uint64_t delta_ms = soonest_wake > now_ms ? soonest_wake - now_ms : 0;
  *timeout_ms = delta_ms > (uint64_t)INT_MAX ? INT_MAX : (int)delta_ms;
  return n_pfds;
}

static bool is_poll_stopped(const struct pollfd *pfds) {
  return (pfds[0].revents & POLLIN) != 0;
}

static void process_poll_result(const struct pollfd *pfds) {
  const uint64_t now_ms = monotonic_ms();
  for (unsigned int i = 0; i < host_count; i++) {
    MonitorHost *host = &monitor_host_list[i];

    if (host->pollfd_slot < 0) {
      continue;
    }

    const short revents = pfds[host->pollfd_slot].revents;
    if (revents != 0) {
      advance_host_poll(host, now_ms);
    } else if (now_ms >= host->iter_deadline_ms) {
      timeout_host_poll(host, now_ms);
    }
  }
}

/**
 * Drives one iteration of the async poll loop:
 *   1. Process IDLE hosts.
 *   2. Build pollfd[].
 *   3. poll() until the earliest deadline (per-host iter_deadline_ms
 *      or next_poll_at_ms).
 *   4. Advance any host whose fd became ready; time out any host whose
 *      deadline expired without ready events.
 *   5. Recompute the master index from the freshly published statuses.
 *
 * Returns false if a stop signal was received, true otherwise.
 */
static bool pump_one_iteration(void) {
  const uint64_t now_ms = monotonic_ms();

  start_and_timeout_hosts(now_ms);

  struct pollfd pfds[MAX_HOSTS + 1];  // +1 for stop pipe
  int timeout_ms;
  const int n_pfds = build_poll_fd(now_ms, pfds, &timeout_ms);

  const int rc = poll(pfds, (nfds_t)n_pfds, timeout_ms);
  if (rc < 0) {
    if (errno == EINTR) {
      return true;
    }
    const int error_number = errno;
    pg_status_log_system_error(
      PG_STATUS_LOG_ERROR, "monitor", error_number, "poll failed"
    );
    return true;
  }

  if (is_poll_stopped(pfds)) {
    return false;
  }

  process_poll_result(pfds);

  recompute_master_index();
  return true;
}

static void mark_pg_monitor_ready(void) {
  pthread_mutex_lock(&start_mutex);
  pg_monitor_ready = true;
  pthread_cond_broadcast(&start_cond);
  pthread_mutex_unlock(&start_mutex);
}

static void warmup(void) {
  bool keep_running = true;
  while (keep_running && !all_hosts_have_polled()) {
    keep_running = pump_one_iteration();
  }

  recompute_master_index();
  if (!keep_running) {
    pg_status_log_fatal("monitor", "warmup interrupted");
  }
}

/**
 * The monitoring thread, which runs continuously and periodically
 * does host checks
 */
static void *pg_monitor_thread(void *arg) {
  set_parameters_from_env();
  init_monitor_host_list();

  warmup();

  mark_pg_monitor_ready();

  bool keep_running = true;
  while (keep_running) {
    keep_running = pump_one_iteration();
  }

  close_all_host_connections();
  return nullptr;
}

/**
 * Starts a host monitoring thread
 */
void start_pg_monitor(void) {
  if (pipe(stop_pipe) != 0) {
    const int error_number = errno;
    pg_status_log_system_fatal(
      "monitor", error_number, "failed to create stop pipe"
    );
  }
  if (
    fcntl(stop_pipe[0], F_SETFD, FD_CLOEXEC) != 0 ||
    fcntl(stop_pipe[1], F_SETFD, FD_CLOEXEC) != 0
  ) {
    const int error_number = errno;
    pg_status_log_system_fatal(
      "monitor", error_number, "failed to set FD_CLOEXEC on stop pipe"
    );
  }

  pthread_mutex_lock(&start_mutex);
  const int started = pthread_create(
    &monitor_tid, nullptr, pg_monitor_thread, nullptr
  );

  if (started != 0) {
    pthread_mutex_unlock(&start_mutex);
    pg_status_log_system_fatal(
      "monitor", started, "failed to start monitor thread"
    );
  }

  while (!pg_monitor_ready) {
    pthread_cond_wait(&start_cond, &start_mutex);
  }
  pthread_mutex_unlock(&start_mutex);
  pg_status_log(PG_STATUS_LOG_INFO, "monitor", "started hosts=%u", host_count);
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
  if (w != 1) {
    const int error_number = w < 0 ? errno : EIO;
    pg_status_log_system_fatal(
      "monitor", error_number, "failed to write stop pipe"
    );
  }

  const int joined = pthread_join(monitor_tid, nullptr);
  if (joined != 0) {
    pg_status_log_system_fatal(
      "monitor", joined, "failed to join monitor thread"
    );
  }

  close(stop_pipe[0]);
  close(stop_pipe[1]);
  stop_pipe[0] = -1;
  stop_pipe[1] = -1;

  pg_status_log(PG_STATUS_LOG_INFO, "monitor", "stopped");
}
