/**
 * Monitoring of postgresql hosts
 */

#include "pg_monitor.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/types.h>
#include <unistd.h>

#include "logger.h"
#include "utils.h"

static pthread_t monitor_tid;

static atomic_bool pg_monitor_ready = false;

bool is_pg_monitor_ready(void) {
  return atomic_load_explicit(&pg_monitor_ready, memory_order_acquire);
}

/**
 * Stop signaling via a pipe.
 */
static int stop_pipe[2] = {-1, -1};

/**
 * Picks the current master by scanning hosts in declared order:
 * first fully-alive master wins, then the first possible_dead master.
 * Dead hosts and replicas are excluded. Always publish the selected index,
 * or -1 when no candidate exists, replacing any previous selection. Without
 * a master, dead hosts are checked every sleep_ms again: any of them may be
 * the one that brings it back.
 */
static void recompute_master_index(void) {
  int master_i = -1;
  int possible_master = -1;
  for (unsigned int i = 0; i < host_count; i++) {
    const MonitorStatus status = atomic_get_status(&monitor_host_list[i]);
    if (!status.alive || !status.master) {
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

  const int selected = master_i != -1 ? master_i : possible_master;
  save_master_index(selected);
  if (selected == -1) {
    cancel_dead_host_backoff();
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
 * Excludes the time the loop was blocked inside libpq on behalf of `caller`
 * from check timing. libpq resolves host names synchronously when it starts a
 * connection and when GSSAPI builds the server principal; without this, one
 * slow DNS answer would be charged to every check in flight, failing hosts
 * whose responses are already waiting, and would use up the spacing before
 * the next connection while the stalled one has not yet begun its handshake.
 */
static void exclude_stall(MonitorHost *caller, const uint64_t stall_ms) {
  if (stall_ms == 0) {
    return;
  }
  for (unsigned int i = 0; i < host_count; i++) {
    MonitorHost *host = &monitor_host_list[i];
    if (
      host->poll_state != HOST_POLL_IDLE &&
      host->poll_state != HOST_POLL_SESSION_WAIT
    ) {
      // SESSION_WAIT accounts for its entire wait when it resumes.
      host->iter_deadline_ms += stall_ms;
    }
  }
  postpone_connection_turn(stall_ms);
  if (caller->poll_state == HOST_POLL_IDLE) {
    // The check ended inside the call, e.g. a failed lookup. Without waiting,
    // the next check could stall again at once and monopolize the loop.
    caller->next_poll_at_ms += stall_ms;
  }
}

/**
 * Resumes queued session setups while the turn is free, starting with the
 * socket that became ready first, whatever its place in the host list. A
 * resumed setup takes the turn; one that fails at once frees it for the
 * next.
 */
static void resume_session_setups(void) {
  for (;;) {
    const ConnectionTurn turn = connection_turn();
    MonitorHost *host = turn.first_session_waiter;
    const uint64_t now_ms = monotonic_ms();
    if (host == nullptr || now_ms < turn.session_setup_due_at_ms) {
      return;
    }
    advance_host_poll(host, now_ms);
    exclude_stall(host, monotonic_ms() - now_ms);
  }
}

/**
 * Starts checks when due and times out active checks whose iter_deadline_ms
 * has passed, then resumes queued session setups. They go last so that a
 * setup timing out after its handshake leaves its pause before the next;
 * no new connection can start ahead of them in the meantime. The connection
 * turn is snapshotted once and again after every change, so the pass stays
 * linear in the number of hosts.
 */
static void start_and_timeout_hosts(void) {
  ConnectionTurn turn = connection_turn();
  for (unsigned int i = 0; i < host_count; i++) {
    MonitorHost *host = &monitor_host_list[i];
    const uint64_t now_ms = monotonic_ms();
    if (host->poll_state == HOST_POLL_IDLE) {
      if (now_ms >= host_poll_due_at_ms(host, &turn)) {
        start_host_poll(host, now_ms);
        exclude_stall(host, monotonic_ms() - now_ms);
        turn = connection_turn();
      }
    } else if (
      host->poll_state != HOST_POLL_SESSION_WAIT &&
      now_ms >= host->iter_deadline_ms
    ) {
      timeout_host_poll(host, now_ms);
      turn = connection_turn();
    }
  }
  resume_session_setups();
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

  uint64_t soonest_wake = UINT64_MAX;
  const ConnectionTurn turn = connection_turn();

  int n_pfds = 1;
  for (unsigned int i = 0; i < host_count; i++) {
    MonitorHost *host = &monitor_host_list[i];

    if (host->poll_state == HOST_POLL_IDLE) {
      const uint64_t due_at_ms = host_poll_due_at_ms(host, &turn);
      if (due_at_ms < soonest_wake) {
        soonest_wake = due_at_ms;
      }
      host->pollfd_slot = -1;
      continue;
    }

    if (host->poll_state == HOST_POLL_SESSION_WAIT) {
      if (turn.session_setup_due_at_ms < soonest_wake) {
        soonest_wake = turn.session_setup_due_at_ms;
      }
      host->pollfd_slot = -1;
      continue;
    }

    const int fd = host_socket(host);
    if (fd < 0) {
      // Connection vanished mid-iteration; force a timeout next tick.
      host->iter_deadline_ms = now_ms;
      host->pollfd_slot = -1;
      soonest_wake = now_ms;
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
  for (unsigned int i = 0; i < host_count; i++) {
    MonitorHost *host = &monitor_host_list[i];

    if (
      host->poll_state == HOST_POLL_IDLE ||
      host->poll_state == HOST_POLL_SESSION_WAIT
    ) {
      continue;
    }

    const uint64_t now_ms = monotonic_ms();
    if (now_ms >= host->iter_deadline_ms) {
      timeout_host_poll(host, now_ms);
    } else if (host->pollfd_slot >= 0 && pfds[host->pollfd_slot].revents != 0) {
      advance_host_poll(host, now_ms);
      exclude_stall(host, monotonic_ms() - now_ms);
    }
  }
}

/**
 * Waits for socket activity or the earliest host deadline, then updates hosts.
 * Returns false if a stop signal was received, true otherwise.
 */
static bool pump_one_iteration(void) {
  start_and_timeout_hosts();

  struct pollfd pfds[MAX_HOSTS + 1];  // +1 for stop pipe
  int timeout_ms;
  const int n_pfds = build_poll_fd(monotonic_ms(), pfds, &timeout_ms);

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

static bool warmup(void) {
  bool keep_running = true;
  while (keep_running && !all_hosts_have_polled()) {
    keep_running = pump_one_iteration();
  }

  recompute_master_index();
  return keep_running;
}

/**
 * The monitoring thread, which runs continuously and periodically
 * does host checks
 */
static void *pg_monitor_thread(void *arg) {
  set_parameters_from_env();
  init_monitor_host_list();

  bool keep_running = warmup();
  if (keep_running) {
    atomic_store_explicit(&pg_monitor_ready, true, memory_order_release);
    pg_status_log(
      PG_STATUS_LOG_INFO, "monitor", "started hosts=%u", host_count
    );
  }
  while (keep_running) {
    keep_running = pump_one_iteration();
  }

  atomic_store_explicit(&pg_monitor_ready, false, memory_order_release);
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

  atomic_store_explicit(&pg_monitor_ready, false, memory_order_release);
  const int started = pthread_create(
    &monitor_tid, nullptr, pg_monitor_thread, nullptr
  );

  if (started != 0) {
    pg_status_log_system_fatal(
      "monitor", started, "failed to start monitor thread"
    );
  }
}

/**
 * Stops a host monitoring thread
 */
void stop_pg_monitor(void) {
  const char b = 1;
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
