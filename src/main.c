#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>

#include "http_server.h"
#include "logger.h"
#include "pg_monitor.h"
#include "pg_status_api.h"
#include "utils.h"

static void block_termination_signals(sigset_t *sigset) {
  sigemptyset(sigset);
  sigaddset(sigset, SIGINT);
  sigaddset(sigset, SIGTERM);

  const int result = pthread_sigmask(SIG_BLOCK, sigset, NULL);
  if (result != 0) {
    pg_status_log_system_fatal(
      "main", result, "failed to block termination signals"
    );
  }
}

static void wait_for_termination_signal(const sigset_t *sigset) {
  int sig;

  const int result = sigwait(sigset, &sig);
  if (result != 0) {
    pg_status_log_system_fatal(
      "main", result, "failed to wait for termination signal"
    );
  }

  switch (sig) {
    case SIGINT:
      pg_status_log(PG_STATUS_LOG_INFO, "main", "received signal=SIGINT");
      break;
    case SIGTERM:
      pg_status_log(PG_STATUS_LOG_INFO, "main", "received signal=SIGTERM");
      break;
    default:
      pg_status_log(PG_STATUS_LOG_INFO, "main", "received signal=%d", sig);
  }
}

static uint16_t get_port(void) {
  const char *env_val = getenv("pg_status__http_port");
  if (env_val && *env_val) {
    return str_to_uint16(env_val);
  }
  return 8000;
}

static const char *get_http_listen_address(void) {
  const char *env_val = getenv("pg_status__http_listen_address");
  if (env_val && *env_val) {
    return env_val;
  }
  return "0.0.0.0";
}

int main(void) {
  sigset_t sigset;
  block_termination_signals(&sigset);

  pg_status_log_init();

  start_pg_monitor();
  HTTPServer *server = start_pg_status_api(
    get_http_listen_address(), get_port()
  );

  wait_for_termination_signal(&sigset);

  stop_http_server(server);
  stop_pg_monitor();
  pg_status_log_shutdown();
  return 0;
}
