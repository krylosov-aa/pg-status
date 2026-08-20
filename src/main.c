#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "http_server.h"
#include "pg_monitor.h"
#include "pg_status_api.h"
#include "utils.h"

static void block_termination_signals(sigset_t *sigset) {
  sigemptyset(sigset);
  sigaddset(sigset, SIGINT);
  sigaddset(sigset, SIGTERM);

  if (pthread_sigmask(SIG_BLOCK, sigset, NULL) != 0) {
    raise_error("pthread_sigmask");
  }
}

static void wait_for_termination_signal(const sigset_t *sigset) {
  int sig;

  if (sigwait(sigset, &sig) == 0) {
    switch (sig) {
      case SIGINT:
        printf("Received SIGINT\n");
        break;
      case SIGTERM:
        printf("Received SIGTERM\n");
        break;
      default:
        printf("Received signal %d\n", sig);
    }
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

  start_pg_monitor();
  HTTPServer *server = start_pg_status_api(
    get_http_listen_address(), get_port()
  );

  wait_for_termination_signal(&sigset);

  stop_http_server(server);
  stop_pg_monitor();
  return 0;
}
